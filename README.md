# LabelSis

Network print server for the **Brother QL-700** label printer, running on an **ESP32-S3** with a self-hosted browser-based label designer.

![Screenshot of the LabelSis label designer](doc/Screenshot.png)

## What it does

Connect the ESP32-S3 DevKitC-1 to the QL-700's USB-B port, flash the firmware from PlatformIO, and use the printer over your local network at `http://labelsis.local/`. No desktop driver, cloud account, or companion app is required.

- Browser-based designer for text, images, barcodes, QR codes, icons, rectangles and CSV-templated batch labels.
- Wi-Fi captive-portal onboarding and storage for up to four wireless networks.
- Live QL-700 status, including media detection and print readiness.
- OTA firmware updates, gated by QL-700 Editor Lite mode or the device BOOT button.
- Native ESP32-S3 USB host connection to the QL-700.
- Python mock server for developing the web editor without flashing hardware.

## Supported hardware and media

This branch supports **only the Brother QL-700** (USB VID:PID `04F9:2042`; Editor Lite mode is detected as `04F9:2049`) and the **ESP32-S3-DevKitC-1-N16R8**. Printing is currently implemented for **DK-22205, 62 mm continuous roll**. Other QL-700 media can be reported by status, but the print endpoint intentionally rejects media whose raster geometry is not implemented.

Connect the DevKitC-1's right-hand native USB-OTG port to the printer's USB-B port. Use the left-hand UART/USB port for PlatformIO upload and serial monitoring. The printer uses its own power supply. See [Hardware](doc/HARDWARE.md).

## Build and upload with PlatformIO

Open the repository in VSCode with the PlatformIO extension, select the `ql700` environment, and run **Build**, **Upload**, or **Monitor**. The equivalent CLI commands are:

```sh
pio run -e ql700
pio run -e ql700 -t upload
pio device monitor -e ql700
```

Copy `main/wifi_credentials.example.h` to `main/wifi_credentials.h` and enter your Wi-Fi credentials before building, or leave it absent and use the `labelsis-setup` onboarding network. The `ql700` PlatformIO environment uses the ESP-IDF framework provided by PlatformIO; the repository no longer maintains a separate standalone `idf.py` build workflow.

See [Install](doc/INSTALL.md) for first flash instructions.

## Development

Run native protocol tests with `./scripts/host-test.sh`; run the firmware build with `pio run -e ql700`. `python scripts/mock-server.py` serves the SPA locally with a simulated QL-700 API. See [Development](doc/DEVELOPMENT.md).

## Security

The HTTP API has **no authentication**: anyone who can reach the device on the network can print, scan Wi-Fi, or rewrite credentials. Run it on a trusted network, **do not expose port 80 to the public internet**, and treat the device like any other unauthenticated LAN appliance.

## License

[MIT](LICENSE). Bundled third-party components keep their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
