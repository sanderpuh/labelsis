#!/usr/bin/env python3
"""
Single-file mock of the LabelSis firmware HTTP API.

Serves the SPA from disk and replies to /api/* with sensible fake
values so the web UI can be developed without flashing the ESP or
even owning the device.

Usage:
    python3 scripts/mock-server.py [--port 8080] [--bind 127.0.0.1]

Then visit http://localhost:8080/ -- same-origin, so no API host
config is needed in the SPA's Settings tab.

Edit STATE / SCAN below to simulate different printer
conditions (cover open, no media, different tape size, etc.).
"""

import argparse
from bundled_fonts import embed_bundled_fonts  # labelsis-fonts-combined-v0.7
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

SPA_DIR = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "components", "pt_app", "spa",
))

# Mutable QL-700 state. Set transport to "waiting" or "editor_lite"
# to exercise the disconnected / maintenance UI states.
STATE = {
    "transport":      "usb_host",
    "printer":        "QL-700",
    "media_width_mm": 62,
    "media_length_mm": 0,
    "media_type":     0x4A,
    "media_kind":     "continuous",
    "error1":         0,
    "error2":         0,
    "status_type":    0,
    "phase_type":     0,
    "phase_number":   0,
    "notification":   0,
    "ready":          True,
    "print_supported": True,
    "fw_version":     "mock-1.0",
}

# Fake nearby Wi-Fi networks for /api/scan.
SCAN_NETWORKS = [
    {"ssid": "Home Wi-Fi",   "rssi": -45, "auth": 3},
    {"ssid": "Guest",        "rssi": -62, "auth": 0},
    {"ssid": "neighbour-3F", "rssi": -78, "auth": 3},
]

# Configured AP list (mirrors NVS on the firmware). Order is priority;
# WIFI_LAST_USED is the SSID the device last successfully connected
# to (tried first on boot).
WIFI_APS = [
    {"ssid": "Home Wi-Fi", "password": "hunter2"},
    {"ssid": "Office",     "password": "officepw"},
]
WIFI_LAST_USED = "Home Wi-Fi"
WIFI_PRESCAN   = False
WIFI_MAX       = 4         # mirror MAX_APS in pt_app.c

CORS = {
    "Access-Control-Allow-Origin":  "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "*",
    "Access-Control-Max-Age":       "3600",
}


class Handler(BaseHTTPRequestHandler):

    # ---- helpers ----------------------------------------------------

    def _send_json(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        for k, v in CORS.items():
            self.send_header(k, v)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_file(self, path, content_type):
        try:
            with open(path, "rb") as f:
                body = f.read()
        except FileNotFoundError:
            self.send_error(404, "not found")
            return
        self.send_response(200)
        for k, v in CORS.items():
            self.send_header(k, v)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        # One-line per request, with our timestamp.
        sys.stderr.write(f"[{time.strftime('%H:%M:%S')}] "
                         f"{self.address_string()} {fmt % args}\n")

    def log_extra(self, line):
        sys.stderr.write(f"           {line}\n")

    # ---- HTTP methods -----------------------------------------------

    def do_OPTIONS(self):
        self.send_response(204)
        for k, v in CORS.items():
            self.send_header(k, v)
        self.end_headers()

    def do_GET(self):
        path = urlparse(self.path).path

        if path in ("/", "/index.html"):
            # Serve bundled font metadata in the offline editor.
            with open(os.path.join(SPA_DIR, "index.html"), encoding="utf-8") as f:
                html = embed_bundled_fonts(f.read(), SPA_DIR)
            payload = html.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)
            return

        if path == "/qrcode.min.js":
            self._send_file(os.path.join(SPA_DIR, "qrcode.min.js"),
                            "application/javascript")
            return

        if path == "/favicon.ico":
            self._send_file(os.path.join(SPA_DIR, "favicon.ico"),
                            "image/x-icon")
            return

        if path == "/setup.html":
            self._send_file(os.path.join(SPA_DIR, "setup.html"), "text/html")
            return

        if path.startswith("/i18n/") and path.endswith(".json"):
            # Whitelist filename to a single segment so the path can't
            # walk out of SPA_DIR. The SPA only ever asks for files
            # like i18n/de.json, never anything fancier.
            name = os.path.basename(path)
            if "/" in name or "\\" in name or name.startswith("."):
                self.send_error(404, "not found")
                return
            self._send_file(os.path.join(SPA_DIR, "i18n", name), "application/json")
            return

        if path == "/fonts/material-icons.woff2":
            self._send_file(os.path.join(SPA_DIR, "fonts", "material-icons.woff2"),
                            "font/woff2")
            return

        if path == "/fonts/material-icons.json":
            # Upstream codepoints.json carries hex strings ("eb8d"); the
            # firmware build converts them to decimal in build_spa_assets.py.
            # Mirror that conversion here so the SPA running against this
            # mock sees exactly the same shape it gets in production.
            with open(os.path.join(SPA_DIR, "fonts",
                                   "material-icons-codepoints.json")) as f:
                raw = json.load(f)
            decimal = {name: int(hex_str, 16) for name, hex_str in raw.items()}
            body = json.dumps(decimal, sort_keys=True,
                              separators=(",", ":")).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)
            return

        if path == "/api/status":
            ota_av = (STATE["transport"] == "editor_lite"
                      or STATE.get("ota_button_gate", False))
            if STATE["transport"] in ("waiting", "editor_lite"):
                self._send_json(503, {
                    "ok": False, "printer": "QL-700",
                    "transport": STATE["transport"],
                    "ota_available": ota_av, "error": "no_printer"})
                return
            self._send_json(200, {**STATE, "ok": True,
                                  "ota_available": ota_av})
            return

        if path == "/api/info":
            self._send_json(200, {
                "ok": True, "printer": "QL-700",
                "fw_version": STATE["fw_version"],
            })
            return

        if path == "/api/scan":
            time.sleep(0.5)        # pretend scanning takes a moment
            self._send_json(200, {"ok": True, "networks": SCAN_NETWORKS})
            return

        if path == "/api/wifi/aps":
            self._send_json(200, {
                "ok":  True,
                "aps": [{"ssid": a["ssid"],
                         "last_used": a["ssid"] == WIFI_LAST_USED}
                        for a in WIFI_APS],
                "prescan": WIFI_PRESCAN,
                "max_aps": WIFI_MAX,
            })
            return

        if path == "/api/ota/status":
            open_ = STATE["transport"] == "editor_lite" or STATE.get("ota_button_gate", False)
            self._send_json(200, {
                "available": open_,
                "reason": "Editor Lite mode" if open_ else "switch QL-700 to Editor Lite mode",
                "running_slot": "ota_0", "next_slot": "ota_1",
                "next_slot_state": "new",
                "app": {"name": "labelsis", "version": STATE["fw_version"]},
            })
            return

        self.send_error(404, "not found")

    def do_POST(self):
        # /api/wifi/aps/{add,delete,order} mutate the in-memory AP list,
        # so the global declaration must cover the whole function.
        global WIFI_APS, WIFI_LAST_USED, WIFI_PRESCAN
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", "0") or 0)
        body = self.rfile.read(length) if length > 0 else b""

        if path == "/api/ql700/print":
            if STATE["transport"] in ("waiting", "editor_lite"):
                self._send_json(503, {"ok": False, "error": "ql700_not_connected"})
                return
            if self.headers.get("X-QL700-Confirm") != "print-62mm":
                self._send_json(403, {"ok": False, "error": "confirmation_required"})
                return
            if length == 0 or length % 90 != 0:
                self._send_json(400, {"ok": False, "error": "invalid_90_byte_raster_length"})
                return
            rows = length // 90
            if not 360 <= rows <= 1500:
                self._send_json(400, {"ok": False, "error": "invalid_raster_rows"})
                return
            if not (STATE["ready"] and STATE["print_supported"]):
                self._send_json(409, {"ok": False, "error": "requires_idle_62mm_continuous_roll"})
                return
            self.log_extra(f"QL-700 raster: {length} bytes ({rows} rows)")
            time.sleep(min(1.0 + rows * 0.003, 6.0))
            self._send_json(202, {
                "ok": True, "rows": rows,
                "printer_complete_verified": False,
                "format": "62mm_continuous",
            })
            return

        if path in ("/api/setup", "/api/wifi/aps/add"):
            try:
                data = json.loads(body) if body else {}
            except json.JSONDecodeError:
                self._send_json(400, {"ok": False, "error": "bad_json"})
                return
            ssid = data.get("ssid")
            password = data.get("password", "")
            if not ssid:
                self._send_json(400, {"ok": False, "error": "missing_ssid"})
                return
            existing = next((a for a in WIFI_APS if a["ssid"] == ssid), None)
            if existing:
                existing["password"] = password
                self.log_extra(f"wifi: replaced password for {ssid!r}")
            elif len(WIFI_APS) >= WIFI_MAX:
                self._send_json(507, {"ok": False, "error": "list_full"})
                return
            else:
                WIFI_APS.append({"ssid": ssid, "password": password})
                self.log_extra(f"wifi: added {ssid!r}")
            self._send_json(200, {"ok": True})
            return

        if path == "/api/wifi/aps/delete":
            try:
                data = json.loads(body) if body else {}
            except json.JSONDecodeError:
                self._send_json(400, {"ok": False, "error": "bad_json"})
                return
            ssid = data.get("ssid")
            if not ssid:
                self._send_json(400, {"ok": False, "error": "missing_ssid"})
                return
            WIFI_APS = [a for a in WIFI_APS if a["ssid"] != ssid]
            if WIFI_LAST_USED == ssid:
                WIFI_LAST_USED = ""
            self.log_extra(f"wifi: deleted {ssid!r}")
            self._send_json(200, {"ok": True})
            return

        if path == "/api/wifi/aps/order":
            wanted = [s.rstrip("\r") for s in body.decode().split("\n") if s.rstrip("\r")]
            existing = {a["ssid"]: a for a in WIFI_APS}
            new_order = [existing[s] for s in wanted if s in existing]
            tail      = [a for a in WIFI_APS if a["ssid"] not in wanted]
            WIFI_APS  = new_order + tail
            self.log_extra(f"wifi: reordered -> {[a['ssid'] for a in WIFI_APS]}")
            self._send_json(200, {"ok": True})
            return

        if path == "/api/wifi/prescan":
            try:
                data = json.loads(body) if body else {}
            except json.JSONDecodeError:
                self._send_json(400, {"ok": False, "error": "bad_json"})
                return
            if "enabled" not in data:
                self._send_json(400, {"ok": False, "error": "missing_enabled"})
                return
            WIFI_PRESCAN = bool(data["enabled"])
            self.log_extra(f"wifi: prescan {'enabled' if WIFI_PRESCAN else 'disabled'}")
            self._send_json(200, {"ok": True})
            return

        if path == "/api/ota":
            if STATE["transport"] != "editor_lite":
                self._send_json(403, {"ok": False, "error": "not_in_editor_lite"})
                return
            # Pretend to "write" the body and reboot. The mock has no
            # real flash; this is just the round-trip the SPA expects.
            self.log_extra(f"ota: would write {length} bytes to inactive slot, then reboot")
            self._send_json(200, {
                "ok":           True,
                "slot":         "ota_1",
                "version":      STATE["fw_version"],
                "reboot_in_ms": 2000,
            })
            return

        if path == "/api/reboot":
            # Same gate as /api/ota -- mock approximates with the
            # same transport=="editor_lite" check. Real firmware uses
            # ota_gate_open() (Editor Lite OR BOOT button override).
            if STATE["transport"] != "editor_lite":
                self._send_json(403, {"ok": False, "error": "gate_closed"})
                return
            self.log_extra("reboot: would soft-reboot")
            self._send_json(202, {"ok": True, "reboot_in_ms": 500})
            return

        if path == "/api/ota/erase-next":
            # Explicit erase of the inactive slot -- recovery path for
            # wedges that the in-handler retry can't fix.
            if STATE["transport"] != "editor_lite":
                self._send_json(403, {"ok": False, "error": "gate_closed"})
                return
            self.log_extra("ota: would erase next slot (mock no-op)")
            self._send_json(200, {"ok": True, "slot": "ota_1", "size": 0x1F0000})
            return

        self.send_error(404, "not found")


def main():
    p = argparse.ArgumentParser(description="LabelSis mock HTTP server")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--bind", default="127.0.0.1",
                   help="interface to bind (use 0.0.0.0 for LAN access)")
    args = p.parse_args()

    if not os.path.isdir(SPA_DIR):
        sys.stderr.write(f"mock-server: SPA dir not found: {SPA_DIR}\n")
        return 1

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f"labelsis-mock listening on http://{args.bind}:{args.port}/")
    print(f"  serving SPA from {SPA_DIR}")
    print(f"  edit STATE at top of {__file__} to simulate other states")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nshutting down")
    return 0


if __name__ == "__main__":
    sys.exit(main())
