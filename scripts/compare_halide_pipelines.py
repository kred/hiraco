#!/usr/bin/env python3
"""
compare_halide_pipelines.py  —  Compare the Halide pipeline output of the
native and the WebAssembly builds.

The two builds do not use the same code generator: the native build links the
object file Halide produced, while the WebAssembly build compiles Halide's C
source output with the Emscripten toolchain (Halide's own `wasmrt` backend
only supports wasm32, and this build needs wasm64).  The vector widths and the
floating point contraction differ, so the results differ in the last bits.

This script measures that difference instead of assuming it.

Usage:
    python compare_halide_pipelines.py <native-check> <wasm-check> [--tmp DIR]

    native-check  path to the native `hiraco-halide-check` executable
    wasm-check    path to the WebAssembly `hiraco-halide-check.js` wrapper
                  (run with Node)
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Tuple

# The WebAssembly build uses a different code generator and a different vector
# width, so the two are not expected to agree bit for bit.  These are the
# limits that were measured on the current toolchains.
MAX_RELATIVE_DELTA = 1e-12
MAX_ULP_DISTANCE = 512


def run_check(executable: str, out_dir: Path) -> Tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    wavelet = out_dir / "wavelet.bin"
    guided = out_dir / "guided.bin"
    command: List[str]
    if executable.endswith(".js"):
        command = ["node", executable]
    else:
        command = [executable]
    command += [str(wavelet), str(guided)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"{executable} failed ({result.returncode}):\n{result.stderr}")
    if not wavelet.exists() or not guided.exists():
        raise SystemExit(f"{executable} did not produce its output files")
    return wavelet, guided


def compare(native: Path, wasm: Path, label: str) -> bool:
    import numpy as np

    a = np.fromfile(native, dtype="<f8")
    b = np.fromfile(wasm, dtype="<f8")
    if a.shape != b.shape:
        print(f"{label}: FAIL shape {a.shape} vs {b.shape}")
        return False
    delta = np.abs(a - b)
    relative = delta / np.maximum(np.abs(a), 1e-300)
    # ULP distance: the number of representable doubles between the two values.
    ulp = np.abs(a.view("i8") - b.view("i8"))
    max_relative = float(relative.max()) if relative.size else 0.0
    max_ulp = int(ulp.max()) if ulp.size else 0
    differing = int(np.count_nonzero(a != b))
    ok = max_relative <= MAX_RELATIVE_DELTA and max_ulp <= MAX_ULP_DISTANCE
    print(f"{label:8s} samples={a.size} differing={differing} "
          f"max_abs={float(delta.max()) if delta.size else 0.0:.3e} "
          f"max_rel={max_relative:.3e} max_ulp={max_ulp} "
          f"-> {'PASS' if ok else 'FAIL'}")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("native_check", help="native hiraco-halide-check executable")
    parser.add_argument("wasm_check", help="WebAssembly hiraco-halide-check.js wrapper")
    parser.add_argument("--tmp", default=None, help="directory for the intermediate files")
    args = parser.parse_args()

    work_dir = Path(args.tmp) if args.tmp else Path(tempfile.mkdtemp(prefix="halide-compare-"))
    work_dir.mkdir(parents=True, exist_ok=True)
    try:
        native_wavelet, native_guided = run_check(args.native_check, work_dir / "native")
        wasm_wavelet, wasm_guided = run_check(args.wasm_check, work_dir / "wasm")
        ok = compare(native_wavelet, wasm_wavelet, "wavelet")
        ok = compare(native_guided, wasm_guided, "guided") and ok
    finally:
        if args.tmp is None:
            shutil.rmtree(work_dir, ignore_errors=True)

    print("RESULT: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
