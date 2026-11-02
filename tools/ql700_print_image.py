
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps, ImageEnhance

# Brother QL-700 / 62 mm continuous tape
PRINTER_DEFAULT = "Brother QL-700"
TAPE_WIDTH_MM = 62
HEAD_DOTS = 720
PRINTABLE_DOTS = 696
SIDE_MARGIN = 12
BYTES_PER_ROW = 90

MIN_ROWS = 360
MAX_ROWS = 1500


def remove_edge_black(image, threshold=35):
    """Remove near-black pixels connected to the image edges."""
    gray = ImageOps.grayscale(image)
    mask = gray.point(
        lambda p: 255 if p <= threshold else 0
    )

    corners = [
        (0, 0),
        (image.width - 1, 0),
        (0, image.height - 1),
        (image.width - 1, image.height - 1),
    ]

    for point in corners:
        if mask.getpixel(point) == 255:
            ImageDraw.floodfill(mask, point, 127, thresh=0)

    background = mask.point(
        lambda p: 255 if p == 127 else 0
    )

    image.paste((255, 255, 255), mask=background)
    return image


def atkinson_dither(image, threshold=128):
    """Convert to black and white using Atkinson error diffusion."""
    gray = ImageOps.grayscale(image)
    width, height = gray.size

    work = [float(p) for p in gray.tobytes()]
    output = bytearray(width * height)

    for y in range(height):
        base = y * width

        for x in range(width):
            index = base + x

            old = work[index]
            new = 255.0 if old >= threshold else 0.0

            output[index] = 255 if new else 0

            error = (old - new) / 8.0

            # Atkinson diffusion pattern
            if x + 1 < width:
                work[index + 1] += error

            if x + 2 < width:
                work[index + 2] += error

            if y + 1 < height:
                if x > 0:
                    work[index + width - 1] += error

                work[index + width] += error

                if x + 1 < width:
                    work[index + width + 1] += error

            if y + 2 < height:
                work[index + 2 * width] += error

    return Image.frombytes("L", (width, height), bytes(output))


def prepare_image(input_path, remove_background=True, bg_threshold=35):
    image = Image.open(input_path)
    image = ImageOps.exif_transpose(image).convert("RGBA")

    # Composite transparent images over white.
    white = Image.new("RGBA", image.size, "white")
    image = Image.alpha_composite(white, image).convert("RGB")

    if remove_background:
        image = remove_edge_black(image, bg_threshold)

    # Fit the image to the printable width.
    content_width = PRINTABLE_DOTS
    content_height = round(
        content_width * image.height / image.width
    )

    # Limit maximum label length.
    if content_height > MAX_ROWS:
        scale = MAX_ROWS / content_height
        content_width = round(content_width * scale)
        content_height = round(content_height * scale)

    content = image.resize(
        (content_width, content_height),
        Image.Resampling.LANCZOS,
    )

    # Increase brightness by 25% before dithering.
    content = ImageEnhance.Brightness(content).enhance(1.10)

    # Minimum continuous-label length.
    total_rows = max(MIN_ROWS, content_height)

    canvas = Image.new(
        "RGB",
        (HEAD_DOTS, total_rows),
        "white",
    )

    left = SIDE_MARGIN + (PRINTABLE_DOTS - content_width) // 2
    top = (total_rows - content_height) // 2

    canvas.paste(content, (left, top))

    return atkinson_dither(canvas), total_rows


def build_raster(preview, rows):
    """
    Convert the upright preview into QL-700 raster rows.

    Each raster row contains 720 dots across the print head.
    The first raster row is the top of the printed image.
    """
    pixels = preview.load()
    raster = bytearray(rows * BYTES_PER_ROW)

    for feed_row in range(rows):
        row_offset = feed_row * BYTES_PER_ROW

        for across_dot in range(
            SIDE_MARGIN,
            HEAD_DOTS - SIDE_MARGIN,
        ):
            if pixels[across_dot, feed_row] < 128:
                byte_index = across_dot >> 3
                bit = 0x80 >> (across_dot & 7)

                raster[row_offset + byte_index] |= bit

    return bytes(raster)


def build_print_job(raster, rows):
    job = bytearray()

    # Invalidate previous printer data.
    job.extend(b"\x00" * 200)

    # Initialize printer and switch to raster mode.
    job.extend(b"\x1b\x40")
    job.extend(b"\x1b\x69\x61\x01")

    # ESC i z: continuous tape, 62 mm wide.
    # Raster count is a little-endian 32-bit value.
    job.extend(b"\x1b\x69\x7a")
    job.extend(bytes((0x86, 0x0A, TAPE_WIDTH_MM, 0x00)))
    job.extend(rows.to_bytes(4, "little"))
    job.extend(b"\x00\x00")

    # Auto-cut enabled.
    job.extend(b"\x1b\x69\x4d\x40")

    # Cut after every label.
    job.extend(b"\x1b\x69\x41\x01")

    # Cut at end.
    job.extend(b"\x1b\x69\x4b\x08")

    # 35-dot feed margin.
    job.extend(b"\x1b\x69\x64\x23\x00")

    # Send raster rows.
    for offset in range(0, len(raster), BYTES_PER_ROW):
        job.extend(b"\x67\x00\x5a")
        job.extend(raster[offset:offset + BYTES_PER_ROW])

    # Print and feed.
    job.append(0x1A)

    return bytes(job)


def list_printers():
    import win32print

    flags = (
        win32print.PRINTER_ENUM_LOCAL
        | win32print.PRINTER_ENUM_CONNECTIONS
    )

    for printer in win32print.EnumPrinters(flags):
        print(printer[2])


def send_raw(printer_name, data):
    import win32print

    handle = win32print.OpenPrinter(printer_name)

    document_open = False
    page_open = False

    try:
        win32print.StartDocPrinter(
            handle,
            1,
            ("QL-700 image", None, "RAW"),
        )
        document_open = True

        win32print.StartPagePrinter(handle)
        page_open = True

        written = win32print.WritePrinter(handle, data)

        if written is not None and written != len(data):
            raise RuntimeError(
                f"Only {written} of {len(data)} bytes were written."
            )

        win32print.EndPagePrinter(handle)
        page_open = False

        win32print.EndDocPrinter(handle)
        document_open = False

    finally:
        if page_open:
            win32print.EndPagePrinter(handle)

        if document_open:
            win32print.EndDocPrinter(handle)

        win32print.ClosePrinter(handle)


def main():
    parser = argparse.ArgumentParser(
        description="Print an image on a Brother QL-700."
    )

    parser.add_argument("image", help="Input PNG or JPG")
    parser.add_argument(
        "--printer",
        default=PRINTER_DEFAULT,
        help="Windows printer queue name",
    )
    parser.add_argument(
        "--keep-background",
        action="store_true",
        help="Do not remove the dark background",
    )
    parser.add_argument(
        "--background-threshold",
        type=int,
        default=35,
        help="Background removal threshold (default 35)",
    )
    parser.add_argument(
        "--preview-only",
        action="store_true",
        help="Create preview without printing",
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="Print without confirmation",
    )
    parser.add_argument(
        "--list-printers",
        action="store_true",
        help="List available Windows printer queues",
    )

    args = parser.parse_args()

    if args.list_printers:
        list_printers()
        return

    input_path = Path(args.image).expanduser().resolve()

    if not input_path.is_file():
        parser.error(f"Image not found: {input_path}")

    if not 0 <= args.background_threshold <= 255:
        parser.error("Background threshold must be between 0 and 255.")

    preview, rows = prepare_image(
    input_path,
    remove_background=not args.keep_background,
    bg_threshold=args.background_threshold,
)

    # Save the preview in the same orientation as the physical print
    preview_path = input_path.with_name(
        input_path.stem + "_ql700_preview.png"
    )
    preview.save(preview_path)

    print(f"Preview saved to: {preview_path.resolve()}")

    # Mirror only the data sent to the printer
    print_image = ImageOps.mirror(preview)
    raster = build_raster(print_image, rows)
    job = build_print_job(raster, rows)

    image_length_mm = rows * 25.4 / 300

    print()
    print("Brother QL-700 image print")
    print("--------------------------")
    print(f"Image:        {input_path.name}")
    print(f"Preview:      {preview_path}")
    print(f"Tape width:   {TAPE_WIDTH_MM} mm")
    print(f"Image width:  {PRINTABLE_DOTS} dots")
    print(f"Image length: {image_length_mm:.1f} mm")
    print(f"Raster rows:  {rows}")
    print(f"Job size:     {len(job):,} bytes")
    print(f"Printer:      {args.printer}")
    print()

    if args.preview_only:
        print("Preview-only mode. Nothing was printed.")
        return

    if not args.yes:
        answer = input("Send this image to the printer? [y/N]: ")
        if answer.strip().lower() not in ("y", "yes"):
            print("Printing cancelled.")
            return

    send_raw(args.printer, job)

    print("Print job sent to Windows spooler.")


if __name__ == "__main__":
    try:
        main()
    except ImportError as exc:
        print(
            "Missing dependency. Run:\n"
            "py -m pip install Pillow pywin32\n"
            f"\n{exc}",
            file=sys.stderr,
        )
        sys.exit(1)
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
