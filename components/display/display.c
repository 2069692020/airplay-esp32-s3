#include "display.h"
#include "rtsp_events.h"

#include "u8g2.h"
#include "u8g2_esp32_hal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "display";

// ---------------------------------------------------------------------------
// Font selection.
//
// The stock u8g2_font_*_tf faces are ASCII-only, so any CJK metadata (which is
// most of it for Chinese-language tracks — iOS sends titles, artists and even
// timed lyrics) renders as blank gaps while the progress bar and the standby
// text keep working.  That reads on the panel as "only a progress bar".
//
// u8g2_font_wqy12_t_gb2312 is a 12 px WenQuanYi face covering 7539 glyphs
// (ISO10646-1, so ASCII and Latin are included alongside CJK), 198 KB of
// .rodata, and its line height matches the 4-line layout's 13-14 px pitch.
// It is compiled by the cjk_font component rather than u8g2's own
// U8G2_USE_LARGE_FONTS switch — see components/cjk_font/CMakeLists.txt.
//
// Only the OLED path gets it; the ST7789/LVGL driver draws with LVGL fonts.
// ---------------------------------------------------------------------------
#if defined(CONFIG_DISPLAY_ENABLED) && !defined(CONFIG_DISPLAY_DRIVER_ST7789)
#define DISPLAY_FONT_CJK
#endif

#ifdef DISPLAY_FONT_CJK
#define FONT_TITLE u8g2_font_wqy12_t_gb2312  // was 7x14B (14 px, ASCII)
#define FONT_BODY u8g2_font_wqy12_t_gb2312  // was 6x13 (13 px, ASCII)
#define FONT_SMALL u8g2_font_wqy12_t_gb2312 // was 5x8 (8 px, ASCII)
#else
#define FONT_TITLE u8g2_font_7x14B_tf
#define FONT_BODY u8g2_font_6x13_tf
#define FONT_SMALL u8g2_font_5x8_tf
#endif

// ============================================================================
// Display state (protected by copy-on-event, read by render task)
// ============================================================================

typedef enum {
  DISPLAY_STATE_STANDBY,
  DISPLAY_STATE_CONNECTED,
  DISPLAY_STATE_PLAYING,
  DISPLAY_STATE_PAUSED,
} display_state_t;

static u8g2_t s_u8g2;

// Written from whichever task emits the RTSP event (RTSP server, Bluedroid
// AVRCP callback, button action task, DACP client), read by the render task.
static portMUX_TYPE s_display_mux = portMUX_INITIALIZER_UNLOCKED;

static struct {
  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
  char album[METADATA_STRING_MAX];
  uint32_t duration_secs;
  uint32_t position_secs;
  display_state_t state;
  bool dirty;           // set by event callback, cleared by render
  int64_t sync_time_us; // esp_timer_get_time() when position was last synced
} s_display;

// Consistent copy of s_display taken once per frame, so widths measured for a
// string always match the string that then gets drawn.
typedef struct {
  char title[METADATA_STRING_MAX];
  char artist[METADATA_STRING_MAX];
  char album[METADATA_STRING_MAX];
  uint32_t duration_secs;
  uint32_t position_secs;
  display_state_t state;
} display_snapshot_t;

// Scroll configuration. The interval is picked so the step lands on a whole
// pixel; a fractional step quantises and reads as jitter.
#define SCROLL_PX_PER_SEC  25  // 3 px per frame at the interval below
#define SCROLL_GAP_PX      30  // pixel gap before text wraps
#define SCROLL_PAUSE_TICKS 3   // pause before scrolling restarts
#define SCROLL_INTERVAL_MS 120 // render interval during active scroll
#define PAUSE_INDICATOR_W  14  // reserved width for "||" + gap

// Baselines for the 128x64 four-line layout.  14 px pitch because the CJK face
// carries 13 px glyphs; the previous ASCII faces were shorter and packed at
// 13/15/14 without touching.  The stack starts at 15 so the first row's glyph
// box clears the top edge, and LINE4_Y sits in the progress bar's cap band
// (draw_progress derives the bar top from the font ascent).
#define LINE1_Y 15
#define LINE2_Y 29
#define LINE3_Y 43
#define LINE4_Y 62

#if defined(CONFIG_DISPLAY_HEIGHT_32)
#define NUM_SCROLL_LINES 1
#else
#define NUM_SCROLL_LINES 3
#endif

static struct {
  int offset[NUM_SCROLL_LINES];
  int pause_ticks[NUM_SCROLL_LINES];
  int step;    // pixels to advance this frame
  bool active; // true if any line is scrolling
} s_scroll;

// Advance on elapsed time, not per frame, so a late frame steps further rather
// than scrolling slower.
static void scroll_tick(void) {
  static int64_t last_us;
  static int carry_subpx;

  const int64_t now = esp_timer_get_time();
  int64_t dt = (last_us == 0) ? (SCROLL_INTERVAL_MS * 1000) : (now - last_us);
  last_us = now;
  if (dt > (4 * SCROLL_INTERVAL_MS * 1000)) {
    dt = 4 * SCROLL_INTERVAL_MS * 1000; // resuming, not catching up
  }
  carry_subpx += (int)((dt * SCROLL_PX_PER_SEC * 256) / 1000000);
  s_scroll.step = carry_subpx / 256;
  carry_subpx -= s_scroll.step * 256;
}

static void scroll_reset(void) {
  memset(&s_scroll, 0, sizeof(s_scroll));
}

static void scroll_restart(void) {
  for (int i = 0; i < NUM_SCROLL_LINES; i++) {
    s_scroll.offset[i] = 0;
    s_scroll.pause_ticks[i] = SCROLL_PAUSE_TICKS;
  }
}

// ============================================================================
// Helpers
// ============================================================================

/**
 * Draw a separator bar in the gap between wrapped copies of the text.
 */
static void draw_scroll_separator(u8g2_t *u8g2, int x, int y) {
  int ascent = u8g2_GetAscent(u8g2);
  int h = ascent;
  u8g2_DrawVLine(u8g2, x, y - h + 1, h);
}

/**
 * Draw a text line with continuous horizontal scrolling when content exceeds
 * max_w.  The text wraps seamlessly: a vertical bar separator and the text's
 * beginning scroll in from the right before the end scrolls off the left,
 * creating a smooth loop with no snap-back.
 */
static void draw_scrolling_line(u8g2_t *u8g2, int idx, int y, const char *str,
                                int max_w) {
  if (!str || !str[0]) {
    return;
  }

  int text_w = u8g2_GetUTF8Width(u8g2, str);

  if (text_w <= max_w) {
    u8g2_DrawUTF8(u8g2, 0, y, str);
    s_scroll.offset[idx] = 0;
    return;
  }

  // Total loop length: text + gap (the gap holds the separator bar)
  int loop_w = text_w + SCROLL_GAP_PX;

  s_scroll.active = true;
  // The clip window exists only to stop scrolling text bleeding past max_w, so
  // its vertical extent just has to cover the line.  Sizing it from
  // ascent/descent is wrong for the CJK face: it reports ascent=8, descent=-2
  // but 13 px-tall glyphs, so the window came out 11 px and shaved the top and
  // bottom off every han character.  Use the face's real glyph height instead.
  int glyph_h = u8g2_GetMaxCharHeight(u8g2);
  int clip_top = y - glyph_h;
  if (clip_top < 0) {
    clip_top = 0;
  }
  int clip_bot = y + glyph_h;
  int disp_h = u8g2_GetDisplayHeight(u8g2);
  if (clip_bot > disp_h - 1) {
    clip_bot = disp_h - 1;
  }
  u8g2_SetClipWindow(u8g2, 0, clip_top, max_w, clip_bot);

  int off = s_scroll.offset[idx];

  // Draw the primary copy
  u8g2_DrawUTF8(u8g2, -off, y, str);
  // Draw separator bar in the gap
  draw_scroll_separator(u8g2, text_w + SCROLL_GAP_PX / 2 - off, y);
  // Draw the wrapped copy (appears from the right)
  u8g2_DrawUTF8(u8g2, loop_w - off, y, str);

  u8g2_SetMaxClipWindow(u8g2);

  if (s_scroll.pause_ticks[idx] > 0) {
    s_scroll.pause_ticks[idx]--;
  } else {
    s_scroll.offset[idx] += s_scroll.step;
    if (s_scroll.offset[idx] >= loop_w) {
      s_scroll.offset[idx] -= loop_w;
    }
  }
}

/**
 * Get the estimated playback position based on wall-clock interpolation.
 * Caller must hold s_display_mux.
 */
static uint32_t estimated_position_locked(void) {
  uint32_t pos = s_display.position_secs;
  if (s_display.state == DISPLAY_STATE_PLAYING && s_display.sync_time_us > 0) {
    int64_t elapsed_us = esp_timer_get_time() - s_display.sync_time_us;
    uint32_t elapsed_secs = (uint32_t)(elapsed_us / 1000000);
    pos += elapsed_secs;
    if (s_display.duration_secs > 0 && pos > s_display.duration_secs) {
      pos = s_display.duration_secs;
    }
  }
  return pos;
}

/**
 * Draw the progress bar: [====>          ] with time on each side.
 */
static void draw_progress(u8g2_t *u8g2, int y, uint32_t pos, uint32_t dur) {
  char pos_str[8], dur_str[8];
  rtsp_format_time_mmss(pos, pos_str, sizeof(pos_str));
  rtsp_format_time_mmss(dur, dur_str, sizeof(dur_str));

  // Measure time string widths
  int pos_w = u8g2_GetUTF8Width(u8g2, pos_str);
  int dur_w = u8g2_GetUTF8Width(u8g2, dur_str);

  // Draw position time on the left
  u8g2_DrawUTF8(u8g2, 0, y, pos_str);
  // Draw duration time on the right.  Use the actual display width rather than
  // a hardcoded 128 — the right-hand time was being placed off-panel on any
  // geometry that is not exactly 128 px wide.
  int disp_w = u8g2_GetDisplayWidth(u8g2);
  u8g2_DrawUTF8(u8g2, disp_w - dur_w, y, dur_str);

  // Progress bar between the time strings.
  //
  // The bar sits on the text's *cap* band, not 5 px above the baseline as it
  // did for the 5x8 ASCII font.  A 12 px face has a taller box, and the old
  // offset buried the bar inside the glyphs.
  int ascent = u8g2_GetAscent(u8g2);
  int bar_h = 5;
  int bar_y = y - ascent + 1;
  int bar_x = pos_w + 3;
  // Leave the same 3 px gap before the duration string.  If the two times plus
  // the gap no longer leave room at this font size, skip the bar and keep the
  // times legible rather than drawing a zero-width frame.
  int bar_w = disp_w - dur_w - 3 - bar_x;

  if (bar_w > 8) {
    // Outline
    u8g2_DrawFrame(u8g2, bar_x, bar_y, bar_w, bar_h);
    // Fill proportional to position
    if (dur > 0 && pos <= dur) {
      int fill = (int)((uint64_t)(bar_w - 2) * pos / dur);
      if (fill > 0) {
        u8g2_DrawBox(u8g2, bar_x + 1, bar_y + 1, fill, bar_h - 2);
      }
    }
  }
}

// ============================================================================
// Rendering
// ============================================================================

static uint8_t s_shadow[1024];
static bool s_shadow_valid;

// Each tile row is a separate blocking I2C transfer, and waiting for those is
// most of a frame, so resend only the rows whose pixels moved.
static void send_dirty_rows(void) {
  const uint8_t *buf = u8g2_GetBufferPtr(&s_u8g2);
  const uint8_t tiles_w = u8g2_GetBufferTileWidth(&s_u8g2);
  const uint8_t tiles_h = u8g2_GetBufferTileHeight(&s_u8g2);
  const size_t row_bytes = (size_t)tiles_w * 8;
  const size_t total = row_bytes * tiles_h;
  const uint32_t errors_before = u8g2_esp32_i2c_error_count();

  if (total > sizeof(s_shadow)) {
    u8g2_SendBuffer(&s_u8g2);
    return;
  }

  if (!s_shadow_valid) {
    u8g2_SendBuffer(&s_u8g2);
  } else {
    uint8_t row = 0;
    while (row < tiles_h) {
      const size_t off = (size_t)row * row_bytes;
      if (memcmp(buf + off, s_shadow + off, row_bytes) == 0) {
        row++;
        continue;
      }
      const uint8_t first = row;
      while (row < tiles_h) {
        const size_t run = (size_t)row * row_bytes;
        if (memcmp(buf + run, s_shadow + run, row_bytes) == 0) {
          break;
        }
        row++;
      }
      u8g2_UpdateDisplayArea(&s_u8g2, 0, first, tiles_w,
                             (uint8_t)(row - first));
    }
  }

  // A dropped transfer leaves the panel showing something other than what was
  // sent, so keep the shadow invalid and repaint in full next frame rather
  // than recording rows that never landed as clean.
  if (u8g2_esp32_i2c_error_count() != errors_before) {
    s_shadow_valid = false;
    return;
  }

  memcpy(s_shadow, buf, total);
  s_shadow_valid = true;
}

static void display_render(void) {
  display_snapshot_t snap;

  taskENTER_CRITICAL(&s_display_mux);
  memcpy(snap.title, s_display.title, sizeof(snap.title));
  memcpy(snap.artist, s_display.artist, sizeof(snap.artist));
  memcpy(snap.album, s_display.album, sizeof(snap.album));
  snap.duration_secs = s_display.duration_secs;
  snap.position_secs = estimated_position_locked();
  snap.state = s_display.state;
  // ⚠️ 清 dirty 必须和上面这次快照**在同一个临界区里**。以前是 display_task()
  //    在调用前先清掉，于是这个顺序会丢更新: 清标志 → 本次快照 → 事件写入新
  //    歌名并置 dirty → 这一帧画的还是旧快照，而 dirty 已经被清成 false →
  //    面板停在旧歌名上，直到下一个不相干的事件来。 (§23.8 记的那条)
  s_display.dirty = false;
  taskEXIT_CRITICAL(&s_display_mux);

  u8g2_ClearBuffer(&s_u8g2);
  s_scroll.active = false;
  scroll_tick();

  switch (snap.state) {
  case DISPLAY_STATE_STANDBY:
    u8g2_SetFont(&s_u8g2, FONT_TITLE);
#if defined(CONFIG_DISPLAY_HEIGHT_32)
    u8g2_DrawUTF8(&s_u8g2, 0, 20, "AirPlay Ready");
#else
    u8g2_DrawUTF8(&s_u8g2, 0, 32, "AirPlay Ready");
#endif
    break;

  case DISPLAY_STATE_CONNECTED:
    u8g2_SetFont(&s_u8g2, FONT_TITLE);
#if defined(CONFIG_DISPLAY_HEIGHT_32)
    u8g2_DrawUTF8(&s_u8g2, 0, 20, "Connected");
#else
    u8g2_DrawUTF8(&s_u8g2, 0, 32, "Connected");
#endif
    break;

  case DISPLAY_STATE_PLAYING:
  case DISPLAY_STATE_PAUSED: {
    bool paused = (snap.state == DISPLAY_STATE_PAUSED);
    int disp_w = u8g2_GetDisplayWidth(&s_u8g2);
    int top_max_w = paused ? disp_w - PAUSE_INDICATOR_W : disp_w;

    // AirPlay 2 keeps delivering position/duration even when no text metadata
    // was ever announced, and a bare progress bar floating on an otherwise
    // blank panel looks broken.  Say what is actually happening instead.
    bool has_meta = snap.title[0] || snap.artist[0] || snap.album[0];
    if (!has_meta) {
      u8g2_SetFont(&s_u8g2, FONT_TITLE);
#if defined(CONFIG_DISPLAY_HEIGHT_32)
      u8g2_DrawUTF8(&s_u8g2, 0, 20, "Playing");
#else
      u8g2_DrawUTF8(&s_u8g2, 0, 32, "Playing");
#endif
      // Paused indicator still applies — it is the only state cue here.
      if (paused) {
        u8g2_SetFont(&s_u8g2, FONT_SMALL);
        int w = u8g2_GetUTF8Width(&s_u8g2, "||");
        u8g2_DrawUTF8(&s_u8g2, disp_w - w, LINE1_Y, "||");
      }
      break;
    }

#if defined(CONFIG_DISPLAY_HEIGHT_32)
    // Compact 2-line layout: "Title - Artist" (scrolling) + progress bar
    char line[METADATA_STRING_MAX * 2 + 4];
    const char *title = snap.title[0] ? snap.title : "---";
    const char *artist = snap.artist[0] ? snap.artist : "";
    if (artist[0]) {
      snprintf(line, sizeof(line), "%s - %s", title, artist);
    } else {
      snprintf(line, sizeof(line), "%s", title);
    }

    u8g2_SetFont(&s_u8g2, FONT_BODY);
    draw_scrolling_line(&s_u8g2, 0, 13, line, top_max_w);

    // Line 2: Progress bar
    u8g2_SetFont(&s_u8g2, FONT_SMALL);
    draw_progress(&s_u8g2, 30, snap.position_secs, snap.duration_secs);
#else
    // Full 4-line layout for 128x64 displays.
    //
    // Baselines are pitched 14 px apart, which is what the CJK face needs: it
    // reports 13 px glyphs, so anything tighter makes consecutive rows touch.
    // The whole stack starts at 15 rather than 13 — at y=13 the 13 px glyph box
    // reaches the top edge, which on a panel with any bezel overlap reads as
    // clipped.
    //
    // Lines are drawn only when they carry text.  A track with no album tag
    // would otherwise leave line 3 empty and push the progress bar's visual
    // weight up against the artist — with the CJK face the same is true for
    // any field iOS simply did not send.
    u8g2_SetFont(&s_u8g2, FONT_TITLE);
    if (snap.title[0]) {
      draw_scrolling_line(&s_u8g2, 0, LINE1_Y, snap.title, top_max_w);
    }

    u8g2_SetFont(&s_u8g2, FONT_BODY);
    if (snap.artist[0]) {
      draw_scrolling_line(&s_u8g2, 1, LINE2_Y, snap.artist, disp_w);
    }

    if (snap.album[0]) {
      draw_scrolling_line(&s_u8g2, 2, LINE3_Y, snap.album, disp_w);
    }

    // Line 4: Progress bar with times
    u8g2_SetFont(&s_u8g2, FONT_SMALL);
    draw_progress(&s_u8g2, LINE4_Y, snap.position_secs, snap.duration_secs);
#endif

    // Paused indicator (top-right)
    if (paused) {
      u8g2_SetFont(&s_u8g2, FONT_SMALL);
      const char *paused_str = "||";
      int w = u8g2_GetUTF8Width(&s_u8g2, paused_str);
      u8g2_DrawUTF8(&s_u8g2, disp_w - w, LINE1_Y, paused_str);
    }
    break;
  }
  }

  send_dirty_rows();
}

// ============================================================================
// RTSP event callback
// ============================================================================

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)user_data;

  taskENTER_CRITICAL(&s_display_mux);

  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    s_display.state = DISPLAY_STATE_CONNECTED;
    memset(s_display.title, 0, sizeof(s_display.title));
    memset(s_display.artist, 0, sizeof(s_display.artist));
    memset(s_display.album, 0, sizeof(s_display.album));
    s_display.duration_secs = 0;
    s_display.position_secs = 0;
    s_display.sync_time_us = 0;
    s_display.dirty = true;
    scroll_reset();
    break;

  case RTSP_EVENT_PLAYING:
    s_display.state = DISPLAY_STATE_PLAYING;
    s_display.sync_time_us = esp_timer_get_time();
    s_display.dirty = true;
    break;

  case RTSP_EVENT_PAUSED:
    // Freeze position at current estimate before pausing
    s_display.position_secs = estimated_position_locked();
    s_display.sync_time_us = 0;
    s_display.state = DISPLAY_STATE_PAUSED;
    s_display.dirty = true;
    break;

  case RTSP_EVENT_DISCONNECTED:
    s_display.state = DISPLAY_STATE_STANDBY;
    memset(s_display.title, 0, sizeof(s_display.title));
    memset(s_display.artist, 0, sizeof(s_display.artist));
    memset(s_display.album, 0, sizeof(s_display.album));
    s_display.duration_secs = 0;
    s_display.position_secs = 0;
    s_display.sync_time_us = 0;
    s_display.dirty = true;
    scroll_reset();
    break;

  case RTSP_EVENT_METADATA:
    if (data) {
      // Detect a real track change so we know when position=0 is legitimate
      // (start of a new track) vs. a spurious mid-song reset from AirPlay.
      bool track_changed = data->metadata.title[0] &&
                           strncmp(s_display.title, data->metadata.title,
                                   METADATA_STRING_MAX) != 0;

      // Only overwrite text fields when the event actually carries them;
      // progress-only updates arrive with zeroed strings.
      if (data->metadata.title[0]) {
        memcpy(s_display.title, data->metadata.title, METADATA_STRING_MAX);
        s_display.title[METADATA_STRING_MAX - 1] = '\0';
      }
      if (data->metadata.artist[0]) {
        memcpy(s_display.artist, data->metadata.artist, METADATA_STRING_MAX);
        s_display.artist[METADATA_STRING_MAX - 1] = '\0';
      }
      if (data->metadata.album[0]) {
        memcpy(s_display.album, data->metadata.album, METADATA_STRING_MAX);
        s_display.album[METADATA_STRING_MAX - 1] = '\0';
      }
      if (data->metadata.duration_secs) {
        s_display.duration_secs = data->metadata.duration_secs;
      }
      // AirPlay occasionally emits position_secs=0 mid-song without a track
      // change, which would reset the progress bar. Only accept 0 on an
      // actual track change; otherwise ignore it and keep the current
      // interpolated position.
      if (data->metadata.position_secs != 0 || track_changed) {
        s_display.position_secs = data->metadata.position_secs;
        s_display.sync_time_us = esp_timer_get_time();
      }
      s_display.dirty = true;
      scroll_restart();
    }
    break;
  }

  taskEXIT_CRITICAL(&s_display_mux);
}

// ============================================================================
// Display task
// ============================================================================

// dirty 现在只由 display_render() 在快照的临界区里清 (见那里), 这里只做
// "要不要重画" 的判断, 所以是**只读**。
static bool display_is_dirty(void) {
  bool d;
  taskENTER_CRITICAL(&s_display_mux);
  d = s_display.dirty;
  taskEXIT_CRITICAL(&s_display_mux);
  return d;
}

static void display_task(void *pvParameters) {
  (void)pvParameters;
  const TickType_t interval = pdMS_TO_TICKS(CONFIG_DISPLAY_UPDATE_MS);
  const TickType_t one_sec = pdMS_TO_TICKS(1000);
  const TickType_t scroll_interval = pdMS_TO_TICKS(SCROLL_INTERVAL_MS);
  TickType_t last_wake = xTaskGetTickCount();

  // Initial render
  display_render();

  while (1) {
    bool is_playing = (s_display.state == DISPLAY_STATE_PLAYING);
    if (s_scroll.active) {
      // A plain delay would add each render to the frame period, tying the
      // scroll speed to I2C timing rather than to the clock.
      if (xTaskDelayUntil(&last_wake, scroll_interval) == pdFALSE) {
        // Already past the deadline, so xTaskDelayUntil did not block. Yield
        // the minimum to keep the loop from spinning, then restart the cadence
        // from now; waiting out another whole interval would stall the scroll
        // exactly when it is already behind.
        vTaskDelay(1);
        last_wake = xTaskGetTickCount();
      }
      display_render();
      continue;
    }
    if (is_playing) {
      vTaskDelay(one_sec);
      last_wake = xTaskGetTickCount();
      display_render();
      continue;
    }
    vTaskDelay(interval);
    last_wake = xTaskGetTickCount();
    if (display_is_dirty()) {
      display_render();
    }
  }
}

// ============================================================================
// Initialization
// ============================================================================

void display_init(void *bus) {
#if defined(CONFIG_DISPLAY_BUS_SPI)
  ESP_LOGI(
      TAG, "Initializing OLED display (SPI: CLK=%d MOSI=%d CS=%d DC=%d RST=%d)",
      CONFIG_DISPLAY_SPI_CLK, CONFIG_DISPLAY_SPI_MOSI, CONFIG_DISPLAY_SPI_CS,
      CONFIG_DISPLAY_SPI_DC, CONFIG_DISPLAY_SPI_RST);

  u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
  if (bus == NULL) {
    hal.bus.spi.clk = CONFIG_DISPLAY_SPI_CLK;
    hal.bus.spi.mosi = CONFIG_DISPLAY_SPI_MOSI;
  }
  hal.bus.spi.cs = CONFIG_DISPLAY_SPI_CS;
  hal.dc = CONFIG_DISPLAY_SPI_DC;
  hal.reset = CONFIG_DISPLAY_SPI_RST;
  u8g2_esp32_hal_init(hal);
  if (bus != NULL) {
    u8g2_esp32_hal_set_spi_host((spi_host_device_t)(intptr_t)bus);
  }

  // Setup u8g2 for the selected display driver and height (SPI)
#if defined(CONFIG_DISPLAY_DRIVER_SH1106)
#if defined(CONFIG_DISPLAY_HEIGHT_32)
  u8g2_Setup_sh1106_128x32_visionox_f(&s_u8g2, U8G2_R0, u8g2_esp32_spi_byte_cb,
                                      u8g2_esp32_gpio_and_delay_cb);
#else
  u8g2_Setup_sh1106_128x64_noname_f(&s_u8g2, U8G2_R0, u8g2_esp32_spi_byte_cb,
                                    u8g2_esp32_gpio_and_delay_cb);
#endif
#elif defined(CONFIG_DISPLAY_DRIVER_SSD1309)
  u8g2_Setup_ssd1309_128x64_noname0_f(&s_u8g2, U8G2_R0, u8g2_esp32_spi_byte_cb,
                                      u8g2_esp32_gpio_and_delay_cb);
#elif defined(CONFIG_DISPLAY_DRIVER_SH1107)
  u8g2_Setup_sh1107_seeed_128x128_f(&s_u8g2, U8G2_R0, u8g2_esp32_spi_byte_cb,
                                    u8g2_esp32_gpio_and_delay_cb);
#else // SSD1306 (default)
#if defined(CONFIG_DISPLAY_HEIGHT_32)
  u8g2_Setup_ssd1306_128x32_univision_f(
      &s_u8g2, U8G2_R0, u8g2_esp32_spi_byte_cb, u8g2_esp32_gpio_and_delay_cb);
#else
  u8g2_Setup_ssd1306_128x64_vcomh0_f(&s_u8g2, U8G2_R0, u8g2_esp32_spi_byte_cb,
                                     u8g2_esp32_gpio_and_delay_cb);
#endif
#endif

#else // I2C (default)
  ESP_LOGI(TAG, "Initializing OLED display (I2C: SDA=%d SCL=%d addr=0x%02x)",
           CONFIG_DISPLAY_I2C_SDA, CONFIG_DISPLAY_I2C_SCL,
           CONFIG_DISPLAY_I2C_ADDR);

  // Bus must be supplied by the board; display cannot init without one.
  if (bus == NULL) {
    ESP_LOGW(TAG, "No I2C bus supplied — display disabled");
    return;
  }
  i2c_master_bus_handle_t i2c_bus = (i2c_master_bus_handle_t)bus;

  // Probe for the display before attempting init. OLED controllers can take
  // up to 100ms to boot after power-on, so retry a few times with delays.
  //
  // Deliberately no i2c_master_bus_reset() between attempts.  Resetting left
  // the controller needing a warm-up transaction before it would talk again,
  // so the probe that followed every reset timed out — no matter whether the
  // timeout was 10 ms or 100 ms, and no matter how many seconds of retries were
  // spent.  The bus scan further down is what gave it away: it never resets,
  // and it found the panel at the same address immediately after the final
  // probe had failed.  A plain delay is enough for the panel to finish booting.
  bool display_found = false;
  for (int attempt = 0; attempt < 15; attempt++) {
    if (attempt > 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (i2c_master_probe(i2c_bus, CONFIG_DISPLAY_I2C_ADDR, 100) == ESP_OK) {
      display_found = true;
      ESP_LOGI(TAG, "Display responded at 0x%02x (attempt %d)",
               CONFIG_DISPLAY_I2C_ADDR, attempt + 1);
      break;
    }
    ESP_LOGD(TAG, "Display probe attempt %d failed", attempt + 1);
  }

  if (!display_found) {
    ESP_LOGW(
        TAG,
        "No OLED display at I2C addr 0x%02x after retries — display disabled",
        CONFIG_DISPLAY_I2C_ADDR);
    i2c_master_bus_reset(i2c_bus);
    // Scan the bus so the caller can see what is actually present
    ESP_LOGW(TAG, "Scanning I2C bus for devices...");
    bool found_any = false;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
      if (i2c_master_probe(i2c_bus, addr, 10) == ESP_OK) {
        ESP_LOGW(TAG, "  found device at 0x%02x", addr);
        found_any = true;
      }
    }
    if (!found_any) {
      ESP_LOGW(TAG, "  no devices found on I2C bus");
    }
    return;
  }

  // Configure the ESP32 HAL for I2C
  u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
  u8g2_esp32_hal_init(hal);
  u8g2_esp32_hal_set_i2c_bus(i2c_bus);

  // Setup u8g2 for the selected display driver and height (I2C)
#if defined(CONFIG_DISPLAY_DRIVER_SH1106)
#if defined(CONFIG_DISPLAY_HEIGHT_32)
  u8g2_Setup_sh1106_i2c_128x32_visionox_f(
      &s_u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
#else
  u8g2_Setup_sh1106_i2c_128x64_noname_f(
      &s_u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
#endif
#elif defined(CONFIG_DISPLAY_DRIVER_SSD1309)
  u8g2_Setup_ssd1309_i2c_128x64_noname0_f(
      &s_u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
#elif defined(CONFIG_DISPLAY_DRIVER_SH1107)
  u8g2_Setup_sh1107_i2c_seeed_128x128_f(
      &s_u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
#else // SSD1306 (default)
#if defined(CONFIG_DISPLAY_HEIGHT_32)
  u8g2_Setup_ssd1306_i2c_128x32_univision_f(
      &s_u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
#else
  u8g2_Setup_ssd1306_i2c_128x64_noname_f(
      &s_u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
#endif
#endif

  // Set I2C address (u8x8 expects left-shifted 7-bit address)
  u8x8_SetI2CAddress(&s_u8g2.u8x8, CONFIG_DISPLAY_I2C_ADDR << 1);
#endif // DISPLAY_BUS

  u8g2_InitDisplay(&s_u8g2);
  u8g2_SetPowerSave(&s_u8g2, 0);

#ifdef CONFIG_DISPLAY_FLIP
  u8g2_SetFlipMode(&s_u8g2, 1);
#endif

  u8g2_ClearBuffer(&s_u8g2);
  u8g2_SendBuffer(&s_u8g2);

  // Initialize state
  s_display.state = DISPLAY_STATE_STANDBY;
  s_display.dirty = true;

  // Register for RTSP events
  rtsp_events_register(on_rtsp_event, NULL);

  // Start display refresh task. Left unpinned: SendBuffer blocks eight times a
  // frame, and the audio core wakes every 5.8 ms, so affinity there is costly.
  xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);

  ESP_LOGI(TAG, "OLED display initialized");
}

// ============================================================================
// 对外读数 —— 网页「正在播放」用，避免开第二份缓存
// ============================================================================

_Static_assert(METADATA_STRING_MAX <= DISPLAY_NOW_PLAYING_MAX,
               "display.h now-playing buffer is smaller than the metadata "
               "source");

bool display_get_now_playing(display_now_playing_t *out) {
  if (!out) {
    return false;
  }

  display_state_t state;

  // 与渲染同一把锁、同一种取法：只 memcpy，不打印不分配，所以 httpd 任务里
  // 调也安全。⚠️ 这里**不清 dirty** —— 那是渲染侧的活，抢着清会让面板停在旧
  // 歌名上（§23.8 记过的同类错误）。
  taskENTER_CRITICAL(&s_display_mux);
  memcpy(out->title, s_display.title, sizeof(out->title));
  memcpy(out->artist, s_display.artist, sizeof(out->artist));
  memcpy(out->album, s_display.album, sizeof(out->album));
  out->duration_secs = s_display.duration_secs;
  out->position_secs = estimated_position_locked();
  state = s_display.state;
  taskEXIT_CRITICAL(&s_display_mux);

  // 显式映射，不靠两个 enum 顺序一致。
  switch (state) {
  case DISPLAY_STATE_PLAYING:
    out->state = DISPLAY_NOW_PLAYING_PLAYING;
    break;
  case DISPLAY_STATE_PAUSED:
    out->state = DISPLAY_NOW_PLAYING_PAUSED;
    break;
  case DISPLAY_STATE_CONNECTED:
    out->state = DISPLAY_NOW_PLAYING_CONNECTED;
    break;
  default:
    out->state = DISPLAY_NOW_PLAYING_STANDBY;
    break;
  }
  return true;
}
