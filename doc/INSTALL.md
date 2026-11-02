# Install

Flash a fresh ESP32-S3 DevKitC-1 from VSCode using PlatformIO. See [HARDWARE.md](HARDWARE.md) for cabling.

## First flash

1. Open this repository in VSCode with the PlatformIO extension installed.
2. (Optional) Copy `main/wifi_credentials.example.h` to `main/wifi_credentials.h` and set `WIFI_SSID` and `WIFI_PASSWORD`. Without this file, the device starts the `labelsis-setup` captive portal.
3. Select the `ql700` PlatformIO environment.
4. Connect the DevKitC-1's **left UART USB-C** port to your PC and click **Build**, then **Upload**.
5. Open **PlatformIO: Serial Monitor** at 115200 baud.

CLI equivalents from the repository root:

```powershell
pio run -e ql700
pio run -e ql700 -t upload
pio device monitor -e ql700
```

Use `pio run -e ql700 -t upload --upload-port COMx` to select a specific Windows COM port.

A healthy boot includes log lines indicating Wi-Fi association, `QL-700 attached over USB`, `labelsis.local`, and HTTP port 80. Then open **http://labelsis.local/** from a device on the same LAN.

## Wi-Fi onboarding

If `main/wifi_credentials.h` is absent or the configured network cannot be reached, the ESP32 starts an open SoftAP named `labelsis-setup`. Connect to it and open `http://192.168.4.1/`, select your network, enter its password, and save. The credentials are stored in NVS.

## Status LED and BOOT button

The DevKitC-1 onboard RGB pixel (GPIO 48) indicates boot, Wi-Fi connection, printer readiness, printing and errors. The BOOT button (GPIO 0) has two runtime gestures: hold for 2–30 seconds and release to toggle the OTA gate; hold for 30 seconds without releasing to wipe Wi-Fi credentials and reboot into setup mode.
