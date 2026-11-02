#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pt_app.h"

/* ESP32-S3 DevKitC-1 pin and LED assignments. */
#include "board.h"

/* Real Wi-Fi creds, gitignored -- copy main/wifi_credentials.example.h
 * to main/wifi_credentials.h and fill in your SSID + password. Falls
 * back to placeholder values when absent so CI / sandbox builds still
 * succeed (they just won't associate). */
#if __has_include("wifi_credentials.h")
#  include "wifi_credentials.h"
#else
#  define WIFI_SSID     "labelsis-build-no-creds"
#  define WIFI_PASSWORD "set-via-wifi_credentials.h"
#endif

static const char *TAG = "labelsis";

void app_main(void)
{
    ESP_LOGI(TAG, "LabelSis print server booting on %s", BOARD_NAME);

    pt_app_config_t cfg = {
        .wifi_ssid              = WIFI_SSID,
        .wifi_password          = WIFI_PASSWORD,
        .http_port              = 80,
        .usb_connect_timeout_ms = 5000,
        .reset_gpio_num         = BOARD_RESET_GPIO,
        /* Status LED is the DevKitC-1 onboard RGB pixel (GPIO 48). */
        .led = {
            .type       = BOARD_LED_TYPE,
            .gpio       = BOARD_LED_GPIO,
            .active_low = BOARD_LED_ACTIVE_LOW,
        },
    };
    pt_app_run(&cfg);

    /* HTTP server runs on its own task; main can sleep. */
    while (1) vTaskDelay(pdMS_TO_TICKS(60000));
}
