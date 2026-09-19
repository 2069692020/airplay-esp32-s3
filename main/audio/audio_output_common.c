/**
 * @file audio_output_common.c
 * @brief Weak defaults for the optional half of the audio output API.
 *
 * Exactly one backend (audio_output.c, _spdif.c, _usb.c, ...) is compiled in,
 * chosen by Kconfig. Callers such as web_server.c and audio_timing.c reference
 * the full API unconditionally, so a backend that does not implement the
 * capability calls used to fail the link — which is why only the I2S build,
 * the one the CI matrix covers, kept working.
 *
 * The defaults below describe a backend with no channel routing and no
 * hardware completion cursor. A backend overrides one by defining it, the
 * same way boards override iot_board_*() in board_common.c. Only genuinely
 * optional entry points belong here: the core ones (init/start/write/...) are
 * deliberately left undefined so a backend missing them still fails loudly.
 */

#include "audio_output.h"

__attribute__((weak)) bool audio_output_get_pipeline_us(int64_t *now_us,
                                                        uint32_t *pipeline_us) {
  (void)now_us;
  (void)pipeline_us;
  // No completion cursor: the timing engine falls back to the modelled
  // hardware latency.
  return false;
}

__attribute__((weak)) uint32_t audio_output_get_underruns(void) {
  return 0;
}

__attribute__((weak)) audio_channel_mode_t
audio_output_cycle_channel_mode(void) {
  return AUDIO_CHANNEL_STEREO;
}

__attribute__((weak)) void
audio_output_set_channel_mode(audio_channel_mode_t mode) {
  (void)mode;
}

__attribute__((weak)) audio_channel_mode_t audio_output_get_channel_mode(void) {
  return AUDIO_CHANNEL_STEREO;
}

// Routing is fixed at stereo, which is what "locked" reports to the web UI so
// it renders the control as unavailable rather than as a working toggle.
__attribute__((weak)) bool audio_output_channel_mode_locked(void) {
  return true;
}

__attribute__((weak)) bool audio_output_channel_mode_in_dsp(void) {
  return false;
}
