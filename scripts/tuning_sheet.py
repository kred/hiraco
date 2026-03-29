#!/usr/bin/env python3
"""
tuning_sheet.py  —  Hiraco parameter-tuning contact sheet generator.

Usage:
    python tuning_sheet.py INPUT.ORF [options]

Each row of the sheet sweeps one parameter while the others stay at their
defaults.  Every tile shows a 512×512 centre-crop of the converted image,
labelled with the changed variable(s).

Requirements:
    pip install pillow rawpy
    (ImageMagick 'convert' is used as fallback if rawpy is absent)
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    raise SystemExit("Missing Pillow.  Run: pip install pillow")

try:
    import rawpy
    import numpy as np
    HAVE_RAWPY = True
except ImportError:
    HAVE_RAWPY = False


# ---------------------------------------------------------------------------
# Parameter catalogue  (name, default, sweep values)
# ---------------------------------------------------------------------------

@dataclass
class Param:
    name: str          # env-var name
    default: Any       # default value used by hiraco when env var is absent
    sweep: List[Any]   # values to try in this parameter's row


PARAMS: List[Param] = [
    # Stage 1 – Wiener deconvolution
    Param("HIRACO_STAGE1_PSF_SIGMA", 2.0,
          [0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0]),
    Param("HIRACO_STAGE1_NSR", 0.1,
          [0.01, 0.03, 0.05, 0.1, 0.15, 0.25, 0.4]),

    # Stage 2 – à trous wavelet
    Param("HIRACO_STAGE2_DENOISE", 0.4,
          [0.0, 0.1, 0.2, 0.4, 0.6, 0.8, 1.2]),
    Param("HIRACO_STAGE2_GAIN0",   1.6,
          [0.8, 1.0, 1.2, 1.6, 2.0, 2.5, 3.0]),
    Param("HIRACO_STAGE2_GAIN1",   1.3,
          [0.8, 1.0, 1.2, 1.3, 1.6, 2.0, 2.5]),
    Param("HIRACO_STAGE2_GAIN2",   1.1,
          [0.5, 0.8, 1.0, 1.1, 1.4, 1.8, 2.2]),
    Param("HIRACO_STAGE2_GAIN3",   1.0,
          [0.5, 0.8, 0.9, 1.0, 1.2, 1.5, 2.0]),

    # Stage 3 – guided filter sharpening
    Param("HIRACO_STAGE3_RADIUS",  4,
          [1, 2, 4, 6, 8, 12, 16]),
    Param("HIRACO_STAGE3_GAIN",   0.5,
          [0.0, 0.2, 0.35, 0.5, 0.7, 1.0, 1.5]),
]

DEFAULTS: Dict[str, Any] = {p.name: p.default for p in PARAMS}

# ---------------------------------------------------------------------------
# Layout constants
# ---------------------------------------------------------------------------

TILE_SIZE   = 512   # pixels per tile (width = height)
LABEL_H     = 40    # height of text strip below each tile
PADDING     = 8     # gap between tiles (and around the sheet)
FONT_SIZE   = 17
ROW_HDR_W   = 280   # width of the left-hand row label column
BG_COLOR    = (28, 28, 28, 255)
LABEL_BG    = (0, 0, 0, 200)
LABEL_FG    = (245, 245, 245, 255)
ROW_HDR_FG  = (210, 210, 210, 255)
DEFAULT_HL  = (60, 200, 100, 255)  # highlight colour for the default value tile


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def fmt_value(v: Any) -> str:
    if isinstance(v, float):
        return f"{v:g}"
    return str(v)


def env_for_variant(changed: Dict[str, Any]) -> Dict[str, str]:
    """Build a full environment dict: defaults + overrides."""
    env = os.environ.copy()
    for name, val in {**DEFAULTS, **changed}.items():
        env[name] = fmt_value(val)
    return env


def run_hiraco(orf: Path, dng: Path, env: Dict[str, str],
               hiraco_bin: Path, timeout: int) -> Optional[str]:
    """Convert orf → dng; return error string or None on success."""
    try:
        result = subprocess.run(
            [str(hiraco_bin), "convert", str(orf), str(dng)],
            env=env, capture_output=True, text=True, timeout=timeout,
        )
        if result.returncode != 0:
            return result.stderr.strip() or f"exit {result.returncode}"
        return None
    except subprocess.TimeoutExpired:
        return "timed out"
    except Exception as exc:  # noqa: BLE001
        return str(exc)


def dng_to_pil(dng: Path) -> Optional[Image.Image]:
    """Convert a DNG file to a PIL Image (RGB).

    Priority: rawpy  →  dcraw  →  ImageMagick
    The hiraco output is a linear DNG (already-demosaiced 16-bit TIFF);
    dcraw with -T -w handles it well without any additional processing.
    """
    # 1. rawpy — best quality, honours camera WB
    if HAVE_RAWPY:
        try:
            with rawpy.imread(str(dng)) as raw:
                rgb = raw.postprocess(
                    use_camera_wb=True,
                    no_auto_bright=True,
                    output_bps=8,
                )
            return Image.fromarray(rgb)
        except Exception:  # noqa: BLE001
            pass

    # 2. dcraw — reliable for linear DNGs, outputs a TIFF next to the input
    if shutil.which("dcraw"):
        tiff = dng.with_suffix(".tiff")
        tiff.unlink(missing_ok=True)
        try:
            r = subprocess.run(
                ["dcraw", "-T", "-w", "-4", str(dng)],
                capture_output=True, timeout=120,
            )
            if r.returncode == 0 and tiff.exists():
                img = Image.open(tiff).convert("RGB")
                img.load()
                tiff.unlink(missing_ok=True)
                return img
        except Exception:  # noqa: BLE001
            pass
        finally:
            tiff.unlink(missing_ok=True)

    # 3. ImageMagick — last resort
    if shutil.which("convert"):
        png = dng.with_suffix(".png")
        try:
            r = subprocess.run(
                ["convert", str(dng), str(png)],
                capture_output=True, timeout=120,
            )
            if r.returncode == 0 and png.exists():
                img = Image.open(png).convert("RGB")
                img.load()
                png.unlink(missing_ok=True)
                return img
        except Exception:  # noqa: BLE001
            pass
        finally:
            png.unlink(missing_ok=True)

    return None


def centre_crop(img: Image.Image, size: int = TILE_SIZE) -> Image.Image:
    w, h = img.size
    x = max((w - size) // 2, 0)
    y = max((h - size) // 2, 0)
    return img.crop((x, y, x + size, y + size)).resize((size, size))


def make_error_tile(size: int = TILE_SIZE) -> Image.Image:
    tile = Image.new("RGB", (size, size), (50, 20, 20))
    draw = ImageDraw.Draw(tile)
    draw.text((size // 2 - 30, size // 2 - 8), "ERROR", fill=(220, 80, 80))
    return tile


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
# Contact-sheet builder
# ---------------------------------------------------------------------------

def build_sheet(
    orf: Path,
    hiraco_bin: Path,
    outfile: Path,
    params: Sequence[Param] = PARAMS,
    timeout: int = 120,
    verbose: bool = False,
) -> None:
    font       = load_font(FONT_SIZE)
    font_small = load_font(max(FONT_SIZE - 4, 10))
    font_hdr   = load_font(max(FONT_SIZE - 2, 10))

    num_rows = len(params)
    num_cols = max(len(p.sweep) for p in params)

    tile_w = TILE_SIZE
    tile_h = TILE_SIZE + LABEL_H

    sheet_w = PADDING + ROW_HDR_W + PADDING + num_cols * (tile_w + PADDING)
    sheet_h = PADDING + num_rows * (tile_h + PADDING)

    sheet = Image.new("RGBA", (sheet_w, sheet_h), BG_COLOR)
    draw  = ImageDraw.Draw(sheet)

    with tempfile.TemporaryDirectory(prefix="hiraco_tuning_") as tmpdir:
        tmp = Path(tmpdir)

        for row_idx, param in enumerate(params):
            y_top = PADDING + row_idx * (tile_h + PADDING)
            y_mid = y_top + TILE_SIZE // 2

            # Row header  (param name + default)
            draw.text(
                (PADDING, y_top + 4),
                param.name.replace("HIRACO_", ""),
                font=font_hdr, fill=ROW_HDR_FG,
            )
            draw.text(
                (PADDING, y_top + FONT_SIZE + 10),
                f"default: {fmt_value(param.default)}",
                font=font_small, fill=(140, 140, 140, 255),
            )

            for col_idx, val in enumerate(param.sweep):
                x_left = PADDING + ROW_HDR_W + PADDING + col_idx * (tile_w + PADDING)

                dng = tmp / f"r{row_idx}_c{col_idx}.dng"
                env = env_for_variant({param.name: val})

                label_top = f"{fmt_value(val)}"

                if verbose:
                    print(f"  [{row_idx},{col_idx}] {param.name}={fmt_value(val)}", flush=True)

                err = run_hiraco(orf, dng, env, hiraco_bin, timeout)
                if err:
                    tile = make_error_tile()
                    label_top = f"{fmt_value(val)} ERR"
                    if verbose:
                        print(f"    → error: {err}", flush=True)
                else:
                    pil = dng_to_pil(dng)
                    if pil is None:
                        tile = make_error_tile()
                        label_top = f"{fmt_value(val)} ?dng"
                    else:
                        tile = centre_crop(pil)

                # Paste the tile
                sheet.paste(tile.convert("RGBA"), (x_left, y_top))

                # Label strip (darkened area at bottom of tile)
                is_default = (val == param.default)
                overlay = Image.new("RGBA", (tile_w, LABEL_H), (0, 0, 0, 0))
                od = ImageDraw.Draw(overlay)
                bg = (40, 120, 60, 210) if is_default else (0, 0, 0, 180)
                od.rectangle([(0, 0), (tile_w, LABEL_H)], fill=bg)
                fg = DEFAULT_HL if is_default else LABEL_FG
                # Centre the label text
                try:
                    bbox = font.getbbox(label_top)
                    tw = bbox[2] - bbox[0]
                except AttributeError:
                    tw = len(label_top) * (FONT_SIZE // 2)
                tx = max((tile_w - tw) // 2, 2)
                od.text((tx, 6), label_top, font=font, fill=fg)
                sheet.alpha_composite(overlay, (x_left, y_top + TILE_SIZE))

    # Convert to RGB before saving JPEG/PNG
    final = sheet.convert("RGB")
    final.save(str(outfile), quality=92 if outfile.suffix.lower() in (".jpg", ".jpeg") else None)
    print(f"\nContact sheet saved → {outfile}  ({sheet_w}×{sheet_h} px)")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Generate a Hiraco parameter-tuning contact sheet.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("source_orf", type=Path,
                    help="Input ORF file (Olympus RAW)")
    ap.add_argument("-o", "--output", type=Path, default=None,
                    help="Output image path (PNG or JPEG). "
                         "Defaults to <orf_stem>_tuning_sheet.jpg next to the ORF.")
    ap.add_argument("--hiraco", type=Path,
                    default=Path(__file__).resolve().parent.parent / "build" / "hiraco",
                    help="Path to the hiraco binary")
    ap.add_argument("--timeout", type=int, default=180,
                    help="Per-conversion timeout in seconds")
    ap.add_argument("--params", nargs="+", metavar="HIRACO_VAR",
                    help="Restrict sheet to these parameters only (e.g. HIRACO_STAGE2_DENOISE)")
    ap.add_argument("-v", "--verbose", action="store_true")
    return ap.parse_args()


def main() -> None:
    args = parse_args()

    orf: Path = args.source_orf.resolve()
    if not orf.exists():
        raise SystemExit(f"ORF not found: {orf}")

    hiraco_bin: Path = args.hiraco.resolve()
    if not hiraco_bin.exists():
        raise SystemExit(f"hiraco binary not found: {hiraco_bin}\n"
                         "Build first or pass --hiraco <path>")

    outfile: Path = args.output or orf.parent / f"{orf.stem}_tuning_sheet.jpg"

    params = PARAMS
    if args.params:
        wanted = {n.upper() for n in args.params}
        params = [p for p in PARAMS if p.name in wanted]
        if not params:
            raise SystemExit(f"None of the requested params found: {args.params}")

    count = sum(len(p.sweep) for p in params)
    print(f"Source : {orf.name}")
    print(f"Binary : {hiraco_bin}")
    print(f"Rows   : {len(params)} parameters")
    print(f"Tiles  : {count} conversions")
    print(f"Output : {outfile}")
    print()

    if not HAVE_RAWPY:
        if shutil.which("dcraw"):
            print("Note   : rawpy not found, using dcraw fallback for DNG→RGB")
        elif shutil.which("convert"):
            print("Note   : rawpy/dcraw not found, using ImageMagick fallback")
        else:
            raise SystemExit(
                "No DNG reader found.\n"
                "Install one: pip install rawpy numpy   OR   brew install dcraw"
            )

    build_sheet(
        orf=orf,
        hiraco_bin=hiraco_bin,
        outfile=outfile,
        params=params,
        timeout=args.timeout,
        verbose=args.verbose,
    )


if __name__ == "__main__":
    main()
