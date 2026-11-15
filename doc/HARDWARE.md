# Hardware

This branch's active hardware profile is the ESP32-S3FH4R2 Super Mini:

| Item | Notes |
|---|---|
| **ESP32-S3FH4R2 Super Mini** | 4 MB QIO flash, 2 MB quad-SPI PSRAM; common board layout has a WS2812 RGB LED on GPIO 48 and BOOT button on GPIO 0. |
| **Brother QL-700** | USB VID:PID `04F9:2042` in normal printer mode. Editor Lite mode enumerates as `04F9:2049` and is detected separately. |
| **USB connection** | Uses the ESP32-S3 native USB-OTG peripheral on GPIO 19 (D−) and GPIO 20 (D+). The Super Mini has one USB-C connector rather than separate UART and OTG connectors. |
| **Printer power supply** | The QL-700 is powered by its own AC adapter, not the ESP32 board. |

## Super Mini USB host and programming

The USB-C data lines are shared by the native USB peripheral. Use the same connector for programming/serial access and USB-host data, as your cabling allows; it is not a separate UART bridge plus OTG port like the DevKitC-1. USB host operation also requires a proper 5 V VBUS source for the downstream printer. The firmware USB-host driver does not switch or generate VBUS, so confirm that the adapter/carrier supplies it before connecting the QL-700. Do not assume that a USB-C cable used for flashing also provides host-mode VBUS.

## Media

Printing is currently supported only with **DK-22205 62 mm continuous roll**. Status detection may identify other media, but other media sizes and die-cut geometries are not enabled in the raster print endpoint yet.
