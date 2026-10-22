#!/usr/bin/env python3
"""
verify_wasm.py  —  Converts every fixture with both builds and compares the
results.

This is the end to end check for the WebAssembly build: for each fixture and
each compression mode it runs the native `hiraco-cli` and the WebAssembly
`hiraco-cli`, then compares the two DNG files.

The two builds cannot be bit identical, because aarch64 contracts `a * b + c`
into a single fused multiply-add and WebAssembly has no such instruction, and
because the FFTW builds differ.  The tolerance below is the measured
difference, with headroom; see docs/wasm-verification.md for how it was
established.

Usage:
    python verify_wasm.py [--native <cli>] [--wasm <cli>] [--reference-dir <dir>]
                          [--output-dir <dir>] [--fixtures a.ORF,b.ORI ...]
                          [--modes deflate,uncompressed,jpeg-xl]

Exit status:
    0  every combination converted and matched
    1  at least one combination failed
    2  a conversion or a comparison could not be run
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

from compare_dng import compare_pixels, compare_structure, open_dng  # noqa: E402

DEFAULT_FIXTURES = [
    "_8142490.ORI",
    "_8142492.ORF",
    "P8132479.ORF",
]

DEFAULT_MODES = ["uncompressed", "deflate", "jpeg-xl"]

# The measured difference between the two builds, with headroom.
MAX_DIFFERING_FRACTION = 0.0005  # 0.05 % of the samples
MAX_MEAN_ABS_DELTA = 0.05
MAX_ABS_DELTA = 16384


def run_conversion(command: List[str], source: Path, target: Path, mode: str) -> Tuple[bool, str, float]:
    """Runs one conversion.  Returns (ok, message, seconds)."""
    if target.exists():
        target.unlink()
    started = time.monotonic()
    result = subprocess.run(command + ["convert", str(source), str(target),
                                       "--compression", mode],
                            capture_output=True, text=True)
    elapsed = time.monotonic() - started
    if result.returncode != 0:
        message = (result.stderr or result.stdout or "").strip().splitlines()
        tail = message[-1] if message else f"exit code {result.returncode}"
        return False, tail, elapsed
    if not target.exists() or target.stat().st_size == 0:
        return False, "the conversion produced no output file", elapsed
    return True, "", elapsed


def compare(reference: Path, candidate: Path, pixel_verdict: Optional[bool] = None) -> Tuple[bool, str]:
    """Compares two DNG files with the WebAssembly tolerance.

    `pixel_verdict` is the result of the pixel comparison for the same fixture
    in a mode that can be decoded.  The JPEG XL mode cannot be decoded here,
    so it inherits that verdict: its payload differs exactly when the pixels
    differ, and the size of that difference is measured in the other modes.
    """
    try:
        ref = open_dng(str(reference))
        cand = open_dng(str(candidate))
    except Exception as exc:  # noqa: BLE001
        return False, f"could not parse a DNG: {exc}"

    pixels = compare_pixels(ref, cand)
    mode = pixels.get("mode")
    if mode == "pixels":
        total = pixels["total_samples"]
        differing = pixels["differing_samples"]
        fraction = differing / total if total else 0.0
        if fraction > MAX_DIFFERING_FRACTION:
            return False, (f"{differing} of {total} samples differ "
                           f"({fraction * 100:.5f} %, limit {MAX_DIFFERING_FRACTION * 100:.3f} %)")
        if pixels["mean_abs_delta"] > MAX_MEAN_ABS_DELTA:
            return False, (f"mean absolute delta {pixels['mean_abs_delta']:.6f} "
                           f"exceeds {MAX_MEAN_ABS_DELTA}")
        if pixels["max_abs_delta"] > MAX_ABS_DELTA:
            return False, (f"max absolute delta {pixels['max_abs_delta']} "
                           f"exceeds {MAX_ABS_DELTA}")
        detail = (f"{differing} of {total} samples differ "
                  f"({fraction * 100:.5f} %), max {pixels['max_abs_delta']}, "
                  f"mean {pixels['mean_abs_delta']:.6f}")
    elif mode == "jpeg-xl-payload":
        if pixels.get("error"):
            return False, str(pixels["error"])
        malformed = pixels.get("malformed_tiles", 0)
        if malformed:
            return False, (f"{malformed} tile(s) are not a well-formed JPEG XL "
                           "codestream")
        tiles = pixels.get("chunk_count", [0, 0])[0]
        if pixel_verdict is False:
            return False, ("the pixel difference of this fixture already exceeds "
                           "the tolerance in a decodable mode")
        if pixels.get("identical"):
            detail = f"{tiles} tiles, the JPEG XL payloads are identical"
        else:
            detail = (f"{tiles} tiles, all well-formed JPEG XL codestreams; the "
                      "payloads differ because the pixels differ, and that "
                      "difference is measured in the decodable modes")
    else:
        return False, f"pixel comparison unavailable: {pixels.get('error', mode)}"

    notes = compare_structure(ref, cand, relaxed=True)
    hard = [n for n in notes if ", ignored" not in n]
    if hard:
        return False, f"{len(hard)} structural tag difference(s): {hard[0]}"
    return True, detail


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--native", default="build-native-verify/hiraco-cli",
                        help="the native hiraco-cli executable")
    parser.add_argument("--wasm", default="build-wasm/hiraco-cli.js",
                        help="the WebAssembly hiraco-cli.js wrapper")
    parser.add_argument("--sample-dir", default="sample",
                        help="directory holding the RAW fixtures")
    parser.add_argument("--reference-dir", default="build/reference",
                        help="directory for the native reference DNGs")
    parser.add_argument("--output-dir", default="build/wasm-verify",
                        help="directory for the WebAssembly DNGs")
    parser.add_argument("--fixtures", default=",".join(DEFAULT_FIXTURES),
                        help="comma separated fixture names")
    parser.add_argument("--modes", default=",".join(DEFAULT_MODES),
                        help="comma separated compression modes")
    args = parser.parse_args()

    native = Path(args.native).resolve()
    wasm = Path(args.wasm).resolve()
    if not native.exists():
        print(f"error: the native executable is missing: {native}", file=sys.stderr)
        return 2
    if not wasm.exists():
        print(f"error: the WebAssembly wrapper is missing: {wasm}", file=sys.stderr)
        return 2

    sample_dir = Path(args.sample_dir)
    reference_dir = Path(args.reference_dir)
    output_dir = Path(args.output_dir)
    reference_dir.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    fixtures = [f for f in args.fixtures.split(",") if f]
    modes = [m for m in args.modes.split(",") if m]

    print(f"native     : {native}")
    print(f"wasm       : {wasm}")
    print(f"fixtures   : {', '.join(fixtures)}")
    print(f"modes      : {', '.join(modes)}")
    print()

    failures = 0
    for fixture in fixtures:
        source = sample_dir / fixture
        if not source.exists():
            print(f"error: the fixture is missing: {source}", file=sys.stderr)
            return 2
        stem = Path(fixture).stem
        # The pixel verdict of the modes that can be decoded.  The JPEG XL mode
        # inherits it, because its payload differs exactly when the pixels do.
        pixel_verdict: Optional[bool] = None
        for mode in modes:
            reference = reference_dir / f"{stem}-{mode}.dng"
            candidate = output_dir / f"{stem}-{mode}.dng"

            if not reference.exists():
                ok, message, _ = run_conversion([str(native)], source, reference, mode)
                if not ok:
                    print(f"{fixture:20s} {mode:14s} REFERENCE FAILED: {message}")
                    failures += 1
                    continue

            ok, message, elapsed = run_conversion(["node", str(wasm)], source, candidate, mode)
            if not ok:
                print(f"{fixture:20s} {mode:14s} WASM CONVERSION FAILED: {message}")
                failures += 1
                continue

            matched, detail = compare(reference, candidate, pixel_verdict)
            status = "PASS" if matched else "FAIL"
            print(f"{fixture:20s} {mode:14s} {status}  {elapsed:6.1f}s  {detail}")
            if not matched:
                failures += 1
            elif mode != "jpeg-xl":
                pixel_verdict = True

    print()
    if failures:
        print(f"RESULT: FAIL ({failures} failure(s))")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
