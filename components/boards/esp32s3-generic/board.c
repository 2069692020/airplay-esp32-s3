/**
 * @file board.c
 * @brief ESP32-S3 Generic board implementation
 *
 * Minimal implementation for generic ESP32-S3 dev boards with external I2S DAC.
 * No board-specific initialization required.
 */

#include "iot_board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char TAG[] = "ESP32S3-Generic";

static bool s_board_initialized = false;

#if defined(CONFIG_DISPLAY_ENABLED) && defined(CONFIG_DISPLAY_BUS_I2C)
// Owned by the board layer, handed to display_init() via iot_board_get_handle().
// display.c does not create a bus itself — it disables the display outright if
// it is passed NULL — so an I2C panel is dead unless the board creates one.
static i2c_master_bus_handle_t s_i2c_disp_bus_handle = NULL;
#endif

#ifdef CONFIG_MUTE_GPIO
static esp_err_t init_mute_gpio(void) {
  if (CONFIG_MUTE_GPIO < 0) {
    return ESP_OK;
  }

  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << CONFIG_MUTE_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&io_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure mute GPIO: %s", esp_err_to_name(err));
    return err;
  }

  // Initialize to unmuted state — set opposite of active level
  gpio_set_level(CONFIG_MUTE_GPIO, !CONFIG_MUTE_GPIO_LEVEL);

  ESP_LOGI(TAG, "Mute GPIO %d initialized (active %s, init %s)",
           CONFIG_MUTE_GPIO, CONFIG_MUTE_GPIO_LEVEL ? "high" : "low",
           CONFIG_MUTE_GPIO_LEVEL ? "low" : "high");
  return ESP_OK;
}
#endif

#if defined(CONFIG_DISPLAY_ENABLED) && defined(CONFIG_DISPLAY_BUS_I2C)
static void init_display_i2c(void) {
  i2c_master_bus_config_t cfg = {
      // -1 lets the driver pick the lowest free controller.  A generic board
      // has no DAC on I2C, so nothing else is competing for one.
      .i2c_port = -1,
      .sda_io_num = CONFIG_DISPLAY_I2C_SDA,
      .scl_io_num = CONFIG_DISPLAY_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };

  esp_err_t err = i2c_new_master_bus(&cfg, &s_i2c_disp_bus_handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG,
             "Failed to initialize display I2C bus (sda=%d, scl=%d): %s — "
             "display will be unavailable",
             CONFIG_DISPLAY_I2C_SDA, CONFIG_DISPLAY_I2C_SCL,
             esp_err_to_name(err));
    s_i2c_disp_bus_handle = NULL;
    return;
  }

  ESP_LOGI(TAG, "Display I2C bus initialized: sda=%d, scl=%d",
           CONFIG_DISPLAY_I2C_SDA, CONFIG_DISPLAY_I2C_SCL);

  // Both lines should sit HIGH when idle.  A LOW line means the pull-ups are
  // too weak or something is actively driving the bus — most often an
  // unpowered panel, or SDA/SCL swapped.  Worth flagging: a swapped pair still
  // reports "bus initialized" fine and only fails later at the probe.
  vTaskDelay(pdMS_TO_TICKS(10));
  int sda_lvl = gpio_get_level(CONFIG_DISPLAY_I2C_SDA);
  int scl_lvl = gpio_get_level(CONFIG_DISPLAY_I2C_SCL);
  if (sda_lvl && scl_lvl) {
    ESP_LOGI(TAG, "Display I2C lines idle-high (SDA=%d SCL=%d) — OK", sda_lvl,
             scl_lvl);
  } else {
    ESP_LOGW(TAG,
             "Display I2C lines not idle-high (SDA=%d SCL=%d) — check that the "
             "panel is powered and SDA/SCL are not swapped",
             sda_lvl, scl_lvl);
  }
}
#endif

const char *iot_board_get_info(void) {
  return BOARD_NAME;
}

bool iot_board_is_init(void) {
  return s_board_initialized;
}

board_res_handle_t iot_board_get_handle(int id) {
  switch (id) {
#if defined(CONFIG_DISPLAY_ENABLED) && defined(CONFIG_DISPLAY_BUS_I2C)
  case BOARD_I2C_DISP_ID:
    return (board_res_handle_t)s_i2c_disp_bus_handle;
#endif
  default:
    return NULL;
  }
}

esp_err_t iot_board_init(void) {
  if (s_board_initialized) {
    ESP_LOGW(TAG, "Board already initialized");
    return ESP_OK;
  }

#ifdef CONFIG_MUTE_GPIO
  esp_err_t err = init_mute_gpio();
  if (err != ESP_OK) {
    return err;
  }
#endif

#if defined(CONFIG_DISPLAY_ENABLED) && defined(CONFIG_DISPLAY_BUS_I2C)
  init_display_i2c();
#endif

  s_board_initialized = true;
  ESP_LOGI(TAG, "Generic board initialized");
  return ESP_OK;
}

esp_err_t iot_board_deinit(void) {
  s_board_initialized = false;
  return ESP_OK;
}
