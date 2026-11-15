/* ESP32-S3FH4R2 Super Mini -- 4 MB QIO flash, 2 MB quad-SPI PSRAM.
 * Common Super Mini layout: onboard WS2812 RGB pixel on GPIO 48;
 * BOOT button on GPIO 0. Verify the LED pin against the exact PCB revision.
 */
#pragma once

#include "pt_led.h"

#define BOARD_NAME            "ESP32-S3FH4R2 Super Mini"
#define BOARD_LED_TYPE        PT_LED_TYPE_RGB
#define BOARD_LED_GPIO        48
#define BOARD_LED_ACTIVE_LOW  false
#define BOARD_RESET_GPIO      0    /* BOOT button -- 5 s hold wipes Wi-Fi creds */
