# Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| No boot log in PlatformIO Serial Monitor | Use the DevKitC-1 **left-hand UART USB-C** port, not the native USB-OTG port connected to the printer. |
| Wi-Fi connection fails | Check `WIFI_SSID` / `WIFI_PASSWORD` in `main/wifi_credentials.h`, or remove that file and use the `labelsis-setup` onboarding network. |
| `transport: waiting` | Check that the QL-700 is powered on and connected to the DevKitC-1's **right-hand USB-OTG** port. |
| `transport: editor_lite` | The QL-700 is in Editor Lite mode. Switch it back to normal printer mode; the firmware retries USB enumeration automatically. |
| Print button remains disabled | Printing requires a connected QL-700, no printer errors, and a ready DK-22205 62 mm continuous roll. Other media geometries are not yet supported for printing. |
| `labelsis.local` does not resolve | Use the device IP from the serial log or your router's DHCP table. Some Windows installations need Bonjour; some networks block mDNS. |
| OTA is unavailable | Switch the QL-700 to Editor Lite mode or hold the device BOOT button for 2–30 seconds and release. The BOOT gesture only toggles OTA after Wi-Fi and HTTP are online. |
| OTA reports an image or flash error | Check the `error` and `esp_err` fields returned by the OTA API. Build the image with `pio run -e ql700` and ensure it is this project's `firmware.bin`. |
| Status LED stays off | The onboard RGB LED is GPIO 48 on the DevKitC-1. Check the board and LED configuration in `boards/devkitc_s3/board.h`. |
| Captive portal does not open automatically | Connect to `labelsis-setup`, then browse to `http://192.168.4.1/` manually. |
| BOOT button reset does not work | Hold GPIO 0 for at least 30 seconds without releasing to wipe Wi-Fi credentials. Releasing between 2 and 30 seconds toggles the OTA gate instead. |
