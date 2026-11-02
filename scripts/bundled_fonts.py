"""Optional firmware-bundled TrueType fonts for LabelSis.

This module is copied to scripts/bundled_fonts.py, then imported by both
build_spa_assets.py and mock-server.py.  An empty fonts/custom/ folder leaves
index.html unchanged. Fonts are embedded into the *generated* HTML as data URLs.
"""
from __future__ import annotations

import base64
import json
from pathlib import Path
import re

FONT_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 _-]{0,79}$")
MAX_FONT_BYTES = 2 * 1024 * 1024


def embedded_font_files(spa_dir: str | Path) -> list[Path]:
    font_dir = Path(spa_dir) / "fonts" / "custom"
    if not font_dir.is_dir():
        return []
    return sorted((p for p in font_dir.iterdir()
                   if p.is_file() and p.suffix.lower() == ".ttf"),
                  key=lambda p: p.name.casefold())


def embed_bundled_fonts(html: str, spa_dir: str | Path) -> str:
    """Inject @font-face and editor registrations; no changes when empty."""
    paths = embedded_font_files(spa_dir)
    if not paths:
        return html

    # Generate safe, human-readable family identifiers that won't collide
    # with operating-system fonts or LabelSis's imported-font slot.
    names = []
    css = []
    seen = set()
    for path in paths:
        display_name = path.stem.strip()
        if not FONT_NAME_RE.fullmatch(display_name):
            raise ValueError(f"Invalid font filename {path.name!r}. "
                             "Use ASCII letters, numbers, spaces, _ and - only.")
        family = f"Bundled: {display_name}"
        if family.casefold() in seen:
            raise ValueError(f"Duplicate bundled font family {family!r}")
        seen.add(family.casefold())
        raw = path.read_bytes()
        if not raw or len(raw) > MAX_FONT_BYTES:
            raise ValueError(f"Font {path.name!r} must be 1 byte to 2 MB")
        # sfnt header validation (regular TTF or TrueType collection excluded).
        if raw[:4] not in (b"\x00\x01\x00\x00", b"true"):
            raise ValueError(f"{path.name!r} is not a standard TrueType .ttf")
        data = base64.b64encode(raw).decode("ascii")
        css.append(f"@font-face {{font-family:{json.dumps(family)};"
                   f"src:url(data:font/ttf;base64,{data}) format('truetype');"
                   "font-style:normal;font-weight:400;font-display:block;}")
        names.append(family)

    if html.count("</head>") != 1 or html.count("/* Sample in the picker") != 1:
        raise ValueError("Expected LabelSis HTML head and FONTS list not found")
    if len(re.findall(r"async\s+function\s+postPrint\s*\(\s*\)\s*\{", html)) != 1:
        raise ValueError("Expected one postPrint() function in LabelSis HTML")
    if 'id="labelsis-bundled-fonts"' in html:
        raise ValueError("Bundled font CSS has already been injected")

    css_tag = '<style id="labelsis-bundled-fonts">\n' + '\n'.join(css) + '\n</style>\n'
    html = html.replace("</head>", css_tag + "</head>", 1)

    # The FONTS array precedes the 'Sample in the picker' comment. Appending
    # the new fonts here makes them render in the existing font picker.
    families = json.dumps(names, ensure_ascii=True)
    js = f'''/* Auto-generated from spa/fonts/custom/*.ttf by bundled_fonts.py. */
const LABELSIS_BUNDLED_FONT_NAMES = {families};
FONTS.push(...LABELSIS_BUNDLED_FONT_NAMES.filter(f => !FONTS.includes(f)));
const LABELSIS_BUNDLED_FONT_READY = Promise.all(
  LABELSIS_BUNDLED_FONT_NAMES.map(async family => {{
    const matches = await document.fonts.load(`16px ${{fontFamilyCSS(family)}}`, "Aa0");
    if (!matches.length) throw new Error(`Could not load bundled font ${{family}}`);
  }})
);
LABELSIS_BUNDLED_FONT_READY.then(
  () => {{ if (typeof redraw === "function") redraw(); }},
  error => console.warn("LabelSis bundled fonts failed to load", error)
);
'''
    html = html.replace("/* Sample in the picker", js + "/* Sample in the picker", 1)

    # Ensure the rasterizer doesn't silently print using a fallback font if
    # the user presses Print before the bundled font has finished loading.
    html = re.sub(r"(async\s+function\s+postPrint\s*\(\s*\)\s*\{)",
                  r"\1\n  await LABELSIS_BUNDLED_FONT_READY;",
                  html, count=1)
    return html
