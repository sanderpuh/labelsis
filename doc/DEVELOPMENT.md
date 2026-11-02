# Development

## Project layout

- `main/` -- firmware entry point and optional Wi-Fi credentials header.
- `boards/devkitc_s3/` -- the single supported ESP32-S3 DevKitC-1 board definition, partition layout and SDK defaults.
- `components/pt_app/` -- Wi-Fi onboarding, HTTP API, OTA handling and embedded web app.
- `components/ql700_protocol/` -- QL-700 status decoding and raster command generation.
- `components/pt_transport/` -- the USB byte transport and ESP32-S3 USB Host backend.
- `components/pt_app/spa/` -- browser-based label editor.
- `test/` -- native QL-700 protocol tests.
- `scripts/mock-server.py` -- local HTTP mock for SPA development.

## Build and tests

PlatformIO is the only supported firmware build workflow. The project uses the ESP-IDF framework as supplied by the PlatformIO Espressif32 platform; the CMake component files are part of that framework integration, not a separate user-facing build path.

```sh
./scripts/host-test.sh
pio run -e ql700
pio run -e ql700 -t upload
pio device monitor -e ql700
```

The host tests compile the same `ql700_protocol` C sources used by the firmware, using the native compiler. `scripts/ci.sh` runs host tests followed by the PlatformIO firmware build.

## SPA development

Run `python scripts/mock-server.py` and open `http://127.0.0.1:8080/`. The mock server serves the SPA from disk and simulates the QL-700 status and print API. For an embedded build, `scripts/platformio_prepare_assets.py` invokes the ESP-IDF CMake asset target inside PlatformIO's build environment; this keeps the generated embedded HTML and gzip assets synchronized with the source SPA.

## Printer support scope

Only Brother QL-700 is supported. USB enumeration accepts QL-700 normal mode (`04F9:2042`) and separately detects its Editor Lite mass-storage mode (`04F9:2049`). Printing currently accepts the 90-byte-per-row QL raster for DK-22205 62 mm continuous media. Other Brother printer families, legacy P-touch print commands, mock firmware transports, host-side libusb transports and PT capture/oracle tools are intentionally not part of this branch.
