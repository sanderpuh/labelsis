# Hardware

This branch has one supported hardware combination:

| Item | Notes |
|---|---|
| **ESP32-S3-DevKitC-1-N16R8** | 16 MB flash, 8 MB octal PSRAM; onboard WS2812 RGB LED on GPIO 48. |
| **Brother QL-700** | USB VID:PID `04F9:2042` in normal printer mode. Editor Lite mode enumerates as `04F9:2049` and is detected separately. |
| **USB-C to USB-B cable** | Connect the DevKitC-1's right-hand native USB-OTG port to the printer's square USB-B port. |
| **USB-C data cable** | Connect the left-hand UART/CP2102N port to the PC for PlatformIO upload and serial monitoring. |
| **Printer power supply** | The QL-700 is powered by its own AC adapter, not the ESP32 board. |

## DevKitC-1 USB ports

The two USB-C ports are not interchangeable. The **left port** is the CP2102N UART bridge used by PlatformIO and the serial monitor. The **right port** is the ESP32-S3 native USB-OTG peripheral and connects to the QL-700. Both can be connected simultaneously.

## Media

Printing is currently supported only with **DK-22205 62 mm continuous roll**. Status detection may identify other media, but other media sizes and die-cut geometries are not enabled in the raster print endpoint yet.
