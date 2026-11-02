#ifndef PT_APP_H
#define PT_APP_H

/*
 * pt_app - QL-700 Wi-Fi print-server firmware for ESP32-S3.
 * Brings up Wi-Fi, the QL-700 USB host transport and HTTP API.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pt_led.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Optional first-boot fallback creds. If NVS has stored creds, those
     * win; otherwise these are tried, and persisted to NVS on first
     * successful STA connect. NULL or empty → skip straight to AP-mode
     * onboarding. */
    const char *wifi_ssid;
    const char *wifi_password;
    uint16_t    http_port;       /* default 80 if 0 */
    uint32_t    usb_connect_timeout_ms;  /* default 5000 if 0 */
    /* Active-low button (with internal pull-up) that, when held for
     * 5 s, wipes Wi-Fi creds from NVS and reboots into AP-mode
     * onboarding. Set to a negative value to disable. GPIO 0 is the
     * BOOT button on most ESP32-S2/S3 devkits. */
    int         reset_gpio_num;
    /* Status LED (optional). Default-initialised type=NONE skips it
     * entirely. See pt_led.h for the cadence/colour table. */
    pt_led_config_t led;
} pt_app_config_t;

/* Bring everything up. Returns ESP_OK once the HTTP server is listening
 * (Wi-Fi connected, transport opened, routes registered). Blocks until
 * Wi-Fi is up or fails after the internal retry budget. After ESP_OK
 * the app keeps running on the HTTP server's tasks; main can return. */
esp_err_t pt_app_run(const pt_app_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* PT_APP_H */
