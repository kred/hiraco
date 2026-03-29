#!/usr/bin/env python3
"""
compare_sheet.py  —  Multi-converter output comparison sheet.

Usage:
    python compare_sheet.py \\
        --file a.tif "Hiraco v1" \\
        --file b.tif "Baseline" \\
        --file c.dng "RawTherapee" \\
        [--crop-x 1024 --crop-y 768] \\
        [--crop-w 512 --crop-h 512] \\
        [-o compare.png]

Generates a PNG with one column per input file.
If --crop-x / --crop-y are omitted, the crop is centred automatically.
Accepts TIF/TIFF (opened directly) and DNG (via rawpy → dcraw → ImageMagick).

Requirements:
    pip install pillow
    brew install dcraw          # recommended DNG fallback
"""
from __future__ import annotations

import argparse
import io
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Optional, Tuple

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    raise SystemExit("Missing Pillow.  Run: pip install pillow")

try:
    import rawpy  # noqa: F401
    import numpy  # noqa: F401
    HAVE_RAWPY = True
except ImportError:
    HAVE_RAWPY = False

# ---------------------------------------------------------------------------
# Layout constants
# ---------------------------------------------------------------------------

PADDING     = 12
HEADER_H    = 52    # column label strip above each tile
FOOTER_H    = 32    # info bar at the very bottom of the sheet
FONT_SIZE   = 17
FONT_SMALL  = 13
BG_COLOR    = (28, 28, 28)
HEADER_BG   = (45, 45, 45)
FOOTER_BG   = (20, 20, 20)
LABEL_FG    = (245, 245, 245)
INFO_FG     = (160, 160, 160)

# ---------------------------------------------------------------------------
# Font loading
# ---------------------------------------------------------------------------

def load_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = [
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/HelveticaNeue.ttc",
        "/Library/Fonts/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    ]
    for path in candidates:
        try:
            return ImageFont.truetype(path, size)
        except (IOError, OSError):
            pass
    return ImageFont.load_default()

# ---------------------------------------------------------------------------
# Image loading
# ---------------------------------------------------------------------------

def _open_tif(path: Path) -> Optional[Image.Image]:
    """Open a TIF/TIFF file directly via PIL."""
    try:
        img = Image.open(path)
        img.load()
        return img.convert("RGB")
    except Exception:  # noqa: BLE001
        return None


def _open_dng(path: Path, brightness: float = 1.0) -> Optional[Image.Image]:
    """Open a DNG file: rawpy → dcraw → ImageMagick."""
    # 1. rawpy
    if HAVE_RAWPY:
        try:
            import rawpy, numpy as np  # noqa: F401
            with rawpy.imread(str(path)) as raw:
                rgb = raw.postprocess(
                    use_camera_wb=True,
                    no_auto_bright=True,
                    output_bps=8,
                    bright=brightness,
                )
            return Image.fromarray(rgb)
        except Exception:  # noqa: BLE001
            pass

    # 2. dcraw  —  pipe 8-bit sRGB PPM to stdout (no temp file, proper gamma)
    #    -b N  scales pixel values by N before clipping (default 1.0)
    if shutil.which("dcraw"):
        try:
            r = subprocess.run(
                ["dcraw", "-w", "-b", str(brightness), "-c", str(path)],
                capture_output=True, timeout=120,
            )
            if r.returncode == 0 and r.stdout:
                img = Image.open(io.BytesIO(r.stdout)).convert("RGB")
                img.load()
                return img
        except Exception:  # noqa: BLE001
            pass

    # 3. ImageMagick
    if shutil.which("convert"):
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
            tmp_path = Path(tmp.name)
        try:
            r = subprocess.run(
                ["convert", str(path), str(tmp_path)],
                capture_output=True, timeout=120,
            )
            if r.returncode == 0 and tmp_path.exists():
                img = Image.open(tmp_path).convert("RGB")
                img.load()
                return img
        except Exception:  # noqa: BLE001
            pass
        finally:
            tmp_path.unlink(missing_ok=True)

    return None


def load_image(path: Path, brightness: float = 1.0,
               verbose: bool = False) -> Optional[Image.Image]:
    suf = path.suffix.lower()
    if suf in (".tif", ".tiff"):
        img = _open_tif(path)
    elif suf == ".dng":
        img = _open_dng(path, brightness=brightness)
    else:
        # Try PIL directly for anything else (e.g. .png, .jpg)
        try:
            img = Image.open(path).convert("RGB")
            img.load()
        except Exception:  # noqa: BLE001
            img = None

    if img is None and verbose:
        print(f"  WARNING: could not load {path.name}", file=sys.stderr)
    return img


def apply_crop(
    img: Image.Image,
    x: Optional[int],
    y: Optional[int],
    crop_w: int,
    crop_h: int,
) -> Image.Image:
    """Crop img to crop_w×crop_h.  If x/y are None, crop from centre."""
    iw, ih = img.size
    if x is None:
        x = max((iw - crop_w) // 2, 0)
    if y is None:
        y = max((ih - crop_h) // 2, 0)
    # Clamp to image bounds
    x = max(0, min(x, max(iw - crop_w, 0)))
    y = max(0, min(y, max(ih - crop_h, 0)))
    return img.crop((x, y, x + crop_w, y + crop_h)).resize((crop_w, crop_h))


def make_error_tile(w: int, h: int) -> Image.Image:
    tile = Image.new("RGB", (w, h), (50, 20, 20))
    draw = ImageDraw.Draw(tile)
    draw.text((w // 2 - 28, h // 2 - 8), "LOAD ERROR", fill=(220, 80, 80))
    return tile

# ---------------------------------------------------------------------------
# Sheet builder
# ---------------------------------------------------------------------------

def build_sheet(
    entries: List[Tuple[Path, str]],   # (path, label)
    outfile: Path,
    crop_x: Optional[int],
    crop_y: Optional[int],
    crop_w: int,
    crop_h: int,
    brightness: float = 1.0,
    verbose: bool = False,
) -> None:
    font      = load_font(FONT_SIZE)
    font_info = load_font(FONT_SMALL)

    n = len(entries)
    tile_w = crop_w
    tile_h = crop_h

    sheet_w = PADDING + n * tile_w + (n - 1) * PADDING + PADDING
    sheet_h = PADDING + HEADER_H + tile_h + PADDING + FOOTER_H

    sheet = Image.new("RGB", (sheet_w, sheet_h), BG_COLOR)
    draw  = ImageDraw.Draw(sheet)

    for col_idx, (path, label) in enumerate(entries):
        x_left = PADDING + col_idx * (tile_w + PADDING)

        # --- header strip ---
        draw.rectangle(
            [(x_left, PADDING), (x_left + tile_w - 1, PADDING + HEADER_H - 1)],
            fill=HEADER_BG,
        )
        # Centre label text
        try:
            bbox = font.getbbox(label)
            tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
        except AttributeError:
            tw, th = len(label) * (FONT_SIZE // 2), FONT_SIZE
        tx = x_left + max((tile_w - tw) // 2, 4)
        ty = PADDING + max((HEADER_H - th) // 2, 4)
        draw.text((tx, ty), label, font=font, fill=LABEL_FG)

        # --- image tile ---
        y_tile = PADDING + HEADER_H
        if verbose:
            print(f"  [{col_idx+1}/{n}] loading {path.name} …", flush=True)

        img = load_image(path, brightness=brightness, verbose=verbose)
        if img is None:
            tile = make_error_tile(tile_w, tile_h)
        else:
            tile = apply_crop(img, crop_x, crop_y, crop_w, crop_h)
            if verbose:
                print(f"        {img.size[0]}×{img.size[1]} source  →  "
                      f"{crop_w}×{crop_h} crop", flush=True)

        sheet.paste(tile, (x_left, y_tile))

    # --- footer info bar ---
    fy = sheet_h - FOOTER_H
    draw.rectangle([(0, fy), (sheet_w - 1, sheet_h - 1)], fill=FOOTER_BG)
    if crop_x is None and crop_y is None:
        pos_str = "centre"
    else:
        pos_str = f"x={crop_x}, y={crop_y}"
    info = f"Crop: {crop_w}×{crop_h} @ {pos_str}  ·  {n} file(s)"
    draw.text((PADDING, fy + (FOOTER_H - FONT_SMALL) // 2), info,
              font=font_info, fill=INFO_FG)

    sheet.save(str(outfile))
    print(f"Saved → {outfile}  ({sheet_w}×{sheet_h} px)")

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Generate a side-by-side converter comparison sheet (PNG).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        epilog=(
            "Example:\n"
            "  compare_sheet.py --file a.tif 'Hiraco' --file b.tif 'RT' "
            "--crop-x 1024 --crop-y 768"
        ),
    )
    ap.add_argument(
        "--file", dest="files", metavar=("PATH", "LABEL"), nargs=2,
        action="append", required=True,
        help="Input file and its label.  Repeat for each converter.",
    )
    ap.add_argument("--crop-x", type=int, default=None,
                    help="X coordinate of crop top-left corner (default: centred)")
    ap.add_argument("--crop-y", type=int, default=None,
                    help="Y coordinate of crop top-left corner (default: centred)")
    ap.add_argument("--crop-w", type=int, default=512,
                    help="Crop width in pixels")
    ap.add_argument("--crop-h", type=int, default=512,
                    help="Crop height in pixels")
    ap.add_argument("-o", "--output", type=Path, default=Path("compare_sheet.png"),
                    help="Output PNG path")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--brightness", type=float, default=1.0,
                    help="Brightness multiplier applied when reading DNG files "
                         "(passed to dcraw -b; 1.5 roughly matches OM Workspace rendering)")
    return ap.parse_args()


def main() -> None:
    args = parse_args()

    entries: List[Tuple[Path, str]] = []
    for path_str, label in args.files:
        p = Path(path_str).resolve()
        if not p.exists():
            raise SystemExit(f"File not found: {p}")
        entries.append((p, label))

    # Warn if only one of crop_x / crop_y is supplied
    if (args.crop_x is None) != (args.crop_y is None):
        raise SystemExit(
            "--crop-x and --crop-y must both be specified, or both omitted "
            "(omitting both centres the crop automatically)."
        )

    outfile: Path = args.output.resolve()

    print(f"Files  : {len(entries)}")
    for p, lbl in entries:
        print(f"         {p.name!r:40s}  [{lbl}]")
    if args.crop_x is None:
        print(f"Crop   : {args.crop_w}×{args.crop_h} @ centre")
    else:
        print(f"Crop   : {args.crop_w}×{args.crop_h} @ ({args.crop_x}, {args.crop_y})")
    print(f"Output : {outfile}")
    print()

    if not HAVE_RAWPY:
        if shutil.which("dcraw"):
            print("Note   : rawpy not available, using dcraw for DNG files")
        elif shutil.which("convert"):
            print("Note   : rawpy/dcraw not found, using ImageMagick for DNG files")
        else:
            # Only fatal if there are actual DNG inputs
            has_dng = any(p.suffix.lower() == ".dng" for p, _ in entries)
            if has_dng:
                raise SystemExit(
                    "No DNG reader found.\n"
                    "Install one: pip install rawpy numpy   OR   brew install dcraw"
                )

    build_sheet(
        entries=entries,
        outfile=outfile,
        crop_x=args.crop_x,
        crop_y=args.crop_y,
        crop_w=args.crop_w,
        crop_h=args.crop_h,
        brightness=args.brightness,
        verbose=args.verbose,
    )


if __name__ == "__main__":
    main()
