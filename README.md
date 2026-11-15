# LabelPrinter

A self-hosted network print server and browser-based label editor for the **Brother QL-700**, built around an **ESP32-S3FH4R2 Super Mini**. The printer is connected over USB; the ESP32 serves the editor and printer API over your local Wi-Fi network.

Open **<http://labelprinter.local/>** from a browser on the same network. LabelPrinter does not require a desktop printer driver, cloud account, or companion app.

## Features

### Three label workflows

- **Custom label** – design labels on a tape-sized canvas. Add and arrange text, images, barcodes, QR codes, icons and rectangles; edit typography and rotation; and manage objects as layers. Use rulers, a grid and centring controls while laying out a design. The editor includes undo/redo, a named-design library, and JSON design import/export. The current design and editor state are automatically saved locally in that browser.
- **Address label** – enter up to five lines of text. Empty lines are ignored, the longest line is fitted to the printable tape width, and the feed length adapts to the number of lines. A live preview shows the result before printing. Address text is saved locally in the browser.
- **Shipping label** – import a PDF, choose the relevant page area, and split it horizontally into two label halves. The selection can be moved and resized with its edge and corner handles; extending it beyond the PDF page adds printable white space. Set the overlap, review both halves, then print them or export them together as a PDF.
- **Printer status and settings** – view live QL-700 connection, readiness, media and error information; manage Wi-Fi and firmware updates from the browser UI.

### CSV templates and batch printing

The **Template & CSV data** panel lets a custom label use values from a CSV table. Put field names in the first row, then add one data row per recipient or item. Use `{field_name}` in text objects to insert a value from the selected row.

For example:

```csv
name,street,postcode,city,$copies
Alice Example,Main Street 12,1012 AB,Amsterdam,2
Bob Sample,High Street 4,3511 CE,Utrecht,1
```

`Print` prints the selected data row. `Print all` processes the data rows as a batch and skips the header. A `$copies` column repeats its row; each `$copies` value is capped at 1,000, and batches larger than ten labels require confirmation. The previous/next controls let you preview the header and data rows.

The following runtime variables can also be used in text objects:

| Variable | Meaning |
|---|---|
| `{$dt\|YYYY-MM-DD}` | Current date in the supplied format |
| `{$dt\|YYYY-MM-DD\|+86400}` | Date shifted by the offset in seconds (here, tomorrow) |
| `{$time\|HH:mm}` | Current time in the supplied format |
| `{$now}` | Current ISO 8601 timestamp (UTC) |
| `{$row}` / `{$row\|+1}` | Zero-based data-row index / one-based row number |
| `{$copy}` | Zero-based copy index within a row (used with `$copies`) |
| `{$seq}` | One-based label number in the batch |
| `{$total}` | Total number of labels in the batch |

Use `\{` and `\}` to insert literal braces. Date/time and numeric variables accept the documented `|` format and signed-offset syntax.

CSV data can be imported and exported from the panel. Designs, CSV editor state, and address-line values are browser-local; they are not synchronized through a cloud service.

## Supported hardware

| Component | Supported configuration |
|---|---|
| Controller | **ESP32-S3FH4R2 Super Mini** |
| Flash | **4 MB QIO flash** |
| PSRAM | **2 MB QSPI PSRAM** |
| Printer | **Brother QL-700 only** |
| USB printer interface | ESP32-S3 native USB OTG, D− on GPIO 19 and D+ on GPIO 20 |
| Status LED / BOOT | Configured for RGB LED GPIO 48 and BOOT GPIO 0 |

The Super Mini has a single USB-C connector rather than separate UART and USB-OTG connectors. Use that connector for programming/serial access when flashing, and connect the QL-700 through a suitable USB host/OTG cable or adapter for normal operation. **USB host mode needs a proper 5 V VBUS supply for the printer-side USB connection.** The firmware does not generate or switch VBUS, so the cable, adapter, or carrier must provide it. The QL-700 also needs its own AC power supply; it is not powered by the ESP32 board.

The QL-700 must be in normal printer mode to print. Its normal USB identity is `04F9:2042`. Editor Lite mode enumerates as `04F9:2049` (mass-storage mode), so printing is unavailable while Editor Lite is selected. That mode can also be used to open the physical-presence gate for OTA updates; see [OTA updates](#ota-firmware-updates).

## Media and print limits

The supported media for printing is **Brother DK-22205, 62 mm continuous tape**. Other QL-700 media may be reported by status, but other widths and die-cut media are not supported by the raster print path.

- Printable tape width used by the editor and PDF export: **58.9 mm** on the 62 mm roll.
- Supported raster feed length: **12.7 mm minimum to 250 mm maximum** per printed section.
- Raster output: 300 dpi, with a 90-byte row payload.

Shipping-label halves are proportionally scaled to fit the 58.9 mm printable width, with a dynamic feed length. The two halves are sent sequentially; the browser waits for the QL-700 to finish feeding/cutting and return to idle before sending the next half.

## Wi-Fi and first start

LabelPrinter advertises **`labelprinter.local`** on the local network. If no saved network can be reached, it starts the open setup access point **`labelprinter-setup`**. Connect to that network and open **<http://192.168.4.1/>** to select Wi-Fi and save the credentials. The device can store up to four networks and tries the most recently used network first; setup mode starts when none of the saved networks connects.

You can also provide credentials at build time by copying `main/wifi_credentials.example.h` to `main/wifi_credentials.h` and entering `WIFI_SSID` and `WIFI_PASSWORD`. Keep your real credentials out of commits. If mDNS does not resolve on your network, use the device IP address shown in the serial log or your router's DHCP client list.

## Build and flash

The firmware is built with **PlatformIO + ESP-IDF**. Open the repository in VS Code with the PlatformIO extension, select the `ql700` environment, and use the **Build**, **Upload**, and **Monitor** actions. The serial monitor runs at **115200 baud**.

Equivalent PlatformIO CLI commands from the repository root:

```sh
pio run -e ql700
pio run -e ql700 -t upload
pio device monitor -e ql700
```

The firmware image used for OTA is created at:

```text
.pio/build/ql700/firmware.bin
```

Do not use a merged flash image for OTA. For USB flashing, connect the Super Mini to the PC and disconnect the printer-side host adapter from that shared USB connection while programming. Once the firmware is running, reconnect the USB host arrangement with the required 5 V VBUS supply.

### Super Mini build profile

The active board profile is in `boards/s3_supermini_fh4r2/`. It sets the 4 MB flash size, QIO flash mode, QSPI PSRAM mode, Super Mini SDK defaults, and a custom partition table with two OTA application slots. Each OTA slot is `0x1E0000` bytes (1.875 MiB), so the application image must fit within that size.

`platformio.ini` currently uses `esp32-s3-devkitc-1` as the generic PlatformIO board identifier, but explicitly overrides its memory and partition settings for the **ESP32-S3FH4R2 Super Mini**. That identifier is a PlatformIO configuration detail; the supported physical target for this branch is the Super Mini, not a DevKitC-1.

## OTA firmware updates

The firmware supports dual-slot OTA updates with bootloader rollback if a new image fails to complete its first successful startup.

The OTA upload is physically gated. Open the gate using either method:

1. Put the QL-700 into **Editor Lite** mode; or
2. Once the device is online, hold the Super Mini **BOOT** button for **2–30 seconds**, then release it.

When the gate is open, the firmware-update panel becomes available in the web UI. Select `.pio/build/ql700/firmware.bin` and start the update. The gate closes again after reboot. For command-line uploads, the repository also includes `scripts/labelsis-ota.py`.

**Important:** holding BOOT for 30 seconds or longer without releasing it triggers the Wi-Fi reset instead: stored Wi-Fi credentials are erased and the device reboots into setup mode. Do not confuse this gesture with the shorter OTA-gate gesture.

## Development and tests

Run the firmware build with `pio run -e ql700`. The native protocol test suite can be run with:

```sh
./scripts/host-test.sh
```

`scripts/ci.sh` runs the host tests followed by the PlatformIO firmware build.

To work on the browser UI without flashing hardware, run the mock server from the repository root:

```sh
python scripts/mock-server.py
```

Then open **<http://127.0.0.1:8080/>**. The mock server serves the SPA and simulates the QL-700 status and print API; it does not print to a physical printer.

Related documentation:

- [Hardware and USB/VBUS notes](doc/HARDWARE.md)
- [Installation](doc/INSTALL.md)
- [Development notes](doc/DEVELOPMENT.md)

## Security

The device's HTTP API has **no authentication**. Anyone who can reach it on the LAN can use the print and Wi-Fi management endpoints; OTA is additionally protected by the physical-presence gate described above. Keep LabelPrinter on a trusted network, do not forward port 80 from the internet, and do not expose the setup access point unnecessarily.

## License

[MIT](LICENSE). Bundled third-party components retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
