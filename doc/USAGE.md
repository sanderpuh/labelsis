# Usage

How to drive the device from the SPA once it's on the LAN.

## The web UI

Open `http://labelsis.local/` (or the device IP) in a browser. Three views:

- **Label** -- the canvas designer. Add text, images, barcodes, QR
  codes, icons, rectangles. Right-click an element for context
  actions; drag the layer list to reorder. The canvas previews at
  true tape geometry, so on-screen pixels map 1:1 to printed dots.
- **Status** -- live printer status (model, tape width, tape colour,
  errors). Polls /api/status every couple of seconds. The dot in the
  title bar is green when a real printer is attached and idle, red
  otherwise.
- **Settings** -- Wi-Fi reconfig, API host (for SPA dev against a
  remote device), UI language.

The current label design and view state auto-save to `localStorage`,
so a reload picks up where you left off. The label library
(top-right) keeps named designs for quick recall.

## CSV-templated batch printing

Open the *Template & CSV data* panel under the Label view. The first
CSV row is treated as field names; subsequent rows are data. Variables
in the design refer to fields:

```
{name}                       -- field "name" from the current row
{$row}                       -- 0-based row index (skips header)
{$copy}                      -- 1-based copy number (used with print all)
{$seq|+1000}                 -- a counter with an offset
{$dt|YYYY-MM-DD}             -- current date in any custom format
{$dt|YYYY-MM-DD|+86400}      -- date offset by N seconds
\{ \}                        -- literal braces (escape)
```

Click *Print all* to run the design once per CSV row. Designs that
expand to >10 labels prompt for confirmation; the hard cap is 1000 to
prevent runaways.

## Editor Lite mode

The QL-700 has a physical Editor Lite switch. In normal printer mode it exposes the USB printer interface used by LabelSis. In **Editor Lite (EL)** mode it exposes mass storage instead, so LabelSis cannot print. The SPA indicates Editor Lite mode when it detects USB ID `04F9:2049`. Switch back to normal printer mode; the ESP32-S3 USB host monitor will detect the QL-700 again automatically.

The Editor Lite state also opens the physical-presence gate for OTA updates. See [OTA.md](OTA.md).
