#!/usr/bin/env bash
#
# build_wasm.sh  —  Configure and build the WebAssembly targets.
#
# The `wasm` CMake preset needs to know where the Emscripten toolchain file
# lives, and that path depends on how Emscripten was installed.  This script
# finds it from `emcc` and passes it to the preset, so the usual invocation is
#
#   ./scripts/build_wasm.sh
#
# Set EMSCRIPTEN_TOOLCHAIN_FILE to override the detection.
#
# With no arguments the script configures and builds `hiraco-cli` and
# `hiraco-wasm`.  Pass target names to build something else:
#
#   ./scripts/build_wasm.sh hiraco-halide-check
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Resolve a path through any chain of symlinks.
readlink_f() {
  local path="$1"
  local dir
  while [[ -L "${path}" ]]; do
    dir="$(cd -P "$(dirname "${path}")" && pwd)"
    path="$(readlink "${path}")"
    [[ "${path}" != /* ]] && path="${dir}/${path}"
  done
  cd -P "$(dirname "${path}")" && printf '%s/%s\n' "$(pwd)" "$(basename "${path}")"
}

if [[ -z "${EMSCRIPTEN_TOOLCHAIN_FILE:-}" ]]; then
  emcc_path="$(command -v emcc || true)"
  if [[ -z "${emcc_path}" ]]; then
    echo "error: emcc is not on PATH; install and activate Emscripten first" >&2
    exit 2
  fi
  # The driver is often a symlink into a versioned directory, so it is
  # resolved first.
  emcc_root="$(dirname "$(readlink_f "${emcc_path}")")"

  # The layout differs between the emsdk and a distribution package: the
  # emsdk keeps the toolchain file under the emcc directory, while a
  # distribution package keeps it next to the bin directory.  Both are
  # searched rather than guessed.
  if [[ -n "${EMSDK:-}" ]]; then
    candidate="${EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
    if [[ -f "${candidate}" ]]; then
      EMSCRIPTEN_TOOLCHAIN_FILE="${candidate}"
    fi
  fi
  if [[ -z "${EMSCRIPTEN_TOOLCHAIN_FILE:-}" ]]; then
    EMSCRIPTEN_TOOLCHAIN_FILE="$(find "${emcc_root}" "${emcc_root}/.." \
        -maxdepth 5 -path '*/Platform/Emscripten.cmake' -print -quit 2>/dev/null || true)"
  fi
  if [[ -z "${EMSCRIPTEN_TOOLCHAIN_FILE:-}" ]]; then
    echo "error: could not locate the Emscripten toolchain file near ${emcc_root}" >&2
    echo "       set EMSCRIPTEN_TOOLCHAIN_FILE to it explicitly" >&2
    exit 2
  fi
fi

echo "Emscripten toolchain: ${EMSCRIPTEN_TOOLCHAIN_FILE}"

cd "${repo_root}"
cmake --preset wasm -DCMAKE_TOOLCHAIN_FILE="${EMSCRIPTEN_TOOLCHAIN_FILE}"

if [[ $# -gt 0 ]]; then
  cmake --build --preset wasm -- "$@"
else
  # The check targets are built as well: they are how the WebAssembly build is
  # compared with the native one.
  cmake --build --preset wasm -- \
    hiraco-cli \
    hiraco-wasm \
    hiraco-capability-check \
    hiraco-halide-check \
    hiraco-libraw-check
fi

echo
echo "Built:"
echo "  build-wasm/hiraco-cli.js               (Node command line converter)"
echo "  build-wasm/hiraco-wasm.js               (browser module)"
echo "  build-wasm/hiraco-capability-check.js   (capability reporting check)"
echo "  build-wasm/hiraco-halide-check.js      (Halide pipeline check)"
echo
echo "Try it:"
echo "  node build-wasm/hiraco-cli.js convert sample/_8142490.ORI /tmp/out.dng --compression deflate"
