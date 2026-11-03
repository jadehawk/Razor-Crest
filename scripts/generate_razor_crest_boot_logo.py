#!/usr/bin/env python3
"""Generate Razor-Crest's boot-only 1bpp helmet art from its PNG master.

The source PNG is grayscale-converted offline (including its blue accents).
The reader's drawImage() accepts only one-bit pixels; dithered flash-resident
images preserve apparent shading without requiring runtime allocations.
"""

from __future__ import annotations

import argparse
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageFont, ImageOps

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/images/HelmetSource.png"
OUTPUT = ROOT / "src/images/RazorCrestBootLogo.h"
PREVIEWS = ROOT / "src/images/helmet-previews"
SIZES = (500, 460)


def grayscale_helmet(source: Image.Image, size: int) -> Image.Image:
    """Neutralize blue/cyan accents, composite alpha on white, and downsample."""
    original = source.convert("RGBA")
    if original.width < size or original.height < size:
        raise ValueError("Helmet source is smaller than a target boot image")
    # Work from the full-resolution master; never scale from an already
    # reduced image (which would lose the sharp lines and small details).
    original = original.resize((size, size), Image.Resampling.LANCZOS)
    converted = bytearray(size * size)
    for i, (r, g, b, a) in enumerate(original.get_flattened_data()):
        intensity = (299 * r + 587 * g + 114 * b + 500) // 1000
        # Cyan wiring was easy to distinguish in color but becomes too light
        # on an e-ink display. Make these features dark graphite instead.
        if g > r + 35 and b > r + 35 and g > 85 and b > 95:
            intensity = min(intensity, 64)
        converted[i] = (intensity * a + 255 * (255 - a) + 127) // 255
    result = Image.frombytes("L", (size, size), bytes(converted))
    return ImageEnhance.Contrast(result).enhance(1.12)


def eink_helmet(gray: Image.Image) -> Image.Image:
    """Use structured shading inside the helmet while keeping edges crisp.

    Floyd-Steinberg spreads noise across outlines and produces visible pixel
    speckles. A small fixed Bayer screen confines gray tone to orderly dots;
    near-black ink and near-white highlights remain solid.
    """
    bayer = (
        (0, 8, 2, 10),
        (12, 4, 14, 6),
        (3, 11, 1, 9),
        (15, 7, 13, 5),
    )
    width, height = gray.size
    pixels = gray.tobytes()
    output = bytearray(len(pixels))
    for y in range(height):
        row = y * width
        for x in range(width):
            value = pixels[row + x]
            if value <= 95:
                output[row + x] = 0
            elif value >= 235:
                output[row + x] = 255
            else:
                threshold = 95 + bayer[y % 4][x % 4] * 9
                output[row + x] = 255 if value >= threshold else 0
    return Image.frombytes("L", (width, height), bytes(output)).convert("1")


def boot_screen_preview(e_ink: Image.Image, size: int) -> Image.Image:
    """Approximate the built-in boot layout for review on the desktop."""
    width, height = (528, 792) if size == 500 else (480, 800)
    canvas = Image.new("RGB", (width, height), "white")
    top = 56 if size == 500 else 82
    canvas.paste(e_ink.convert("RGB"), ((width - size) // 2, top))
    draw = ImageDraw.Draw(canvas)

    def draw_label(message: str, y: int, font_size: int) -> None:
        font_path = Path("C:/Windows/Fonts/arialbd.ttf")
        font = ImageFont.truetype(str(font_path), font_size) if font_path.exists() else ImageFont.load_default()
        bounds = draw.textbbox((0, 0), message, font=font)
        draw.text(((width - (bounds[2] - bounds[0])) // 2, y), message, fill="black", font=font)

    draw_label("This Is the Way", top + size + 16, 25)
    draw_label("RAZOR-CREST", top + size + 78, 17)
    draw_label("Booting...", top + size + 110, 15)
    return canvas


def sleep_screen_preview(e_ink: Image.Image, size: int, dark: bool) -> Image.Image:
    """Approximate both native built-in sleep screen modes for device review."""
    width, height = (528, 792) if size == 500 else (480, 800)
    canvas = Image.new("RGB", (width, height), "white")
    tagline_height, brand_height, sleep_height = 25, 17, 15
    block_height = size + 16 + tagline_height + 28 + brand_height + 6 + sleep_height
    top = max(8, (height - block_height) // 2)
    canvas.paste(e_ink.convert("RGB"), ((width - size) // 2, top))
    draw = ImageDraw.Draw(canvas)
    font_path = Path("C:/Windows/Fonts/arialbd.ttf")

    def draw_label(message: str, y: int, font_size: int) -> None:
        font = ImageFont.truetype(str(font_path), font_size) if font_path.exists() else ImageFont.load_default()
        bounds = draw.textbbox((0, 0), message, font=font)
        draw.text(((width - (bounds[2] - bounds[0])) // 2, y), message, fill="black", font=font)

    tagline_y = top + size + 16
    draw_label("This Is the Way", tagline_y, tagline_height)
    brand_y = tagline_y + tagline_height + 28
    draw_label("RAZOR-CREST", brand_y, brand_height)
    draw_label("Sleeping...", brand_y + brand_height + 6, sleep_height)
    return ImageOps.invert(canvas) if dark else canvas


def build_assets() -> tuple[str, dict[str, bytes]]:
    original = Image.open(SOURCE)
    if original.size != (1254, 1254) or original.mode != "RGBA":
        raise ValueError(f"Unexpected helmet master: {original.size}, {original.mode}")

    lines = [
        "#pragma once",
        "#include <cstdint>",
        "",
        "// Generated by scripts/generate_razor_crest_boot_logo.py; do not hand-edit.",
        "// 1bpp (white=1), pre-rotated CCW for GfxRenderer::drawImage().",
        "",
    ]
    images = {}
    for size in SIZES:
        gray = grayscale_helmet(original, size)
        # The framebuffer is monochrome. Keep high-contrast outlines solid and
        # represent metallic gradients with a repeatable ordered halftone.
        e_ink = eink_helmet(gray)
        for filename, img in (
            (f"Helmet-Grayscale-{size}.png", gray),
            (f"Helmet-Eink-{size}.png", e_ink),
            ("Boot-Preview-X3.png" if size == 500 else "Boot-Preview-X4Pro.png", boot_screen_preview(e_ink, size)),
            ("Sleep-Preview-X3-Light.png" if size == 500 else "Sleep-Preview-X4Pro-Light.png",
             sleep_screen_preview(e_ink, size, False)),
            ("Sleep-Preview-X3-Dark.png" if size == 500 else "Sleep-Preview-X4Pro-Dark.png",
             sleep_screen_preview(e_ink, size, True)),
        ):
            data = BytesIO()
            img.save(data, format="PNG", optimize=True)
            images[filename] = data.getvalue()

        bitmap = e_ink.rotate(90, expand=False)
        pixels = bitmap.tobytes()
        assert len(pixels) == ((size + 7) // 8) * size
        lines.append(f"inline constexpr uint8_t RAZOR_CREST_BOOT_LOGO_{size}[] = {{")
        for i in range(0, len(pixels), 20):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in pixels[i : i + 20]) + ",")
        lines.extend(("};", ""))

    return "\n".join(lines), images


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Verify header and previews are up-to-date")
    args = parser.parse_args()

    expected_header, expected_images = build_assets()
    if args.check:
        correct_header = OUTPUT.exists() and OUTPUT.read_text(encoding="utf-8") == expected_header
        correct_images = all(
            (PREVIEWS / name).exists() and (PREVIEWS / name).read_bytes() == content
            for name, content in expected_images.items()
        )
        if not (correct_header and correct_images):
            print("Razor-Crest helmet assets are out of date; rerun the generator")
            return 1
        print("Verified grayscale previews and 500/460px boot bitmaps")
        return 0

    PREVIEWS.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(expected_header, encoding="utf-8", newline="\n")
    for name, content in expected_images.items():
        (PREVIEWS / name).write_bytes(content)
    print("Generated 500/460px grayscale previews and flash-resident 1bpp boot bitmaps")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
