#!/usr/bin/env bash
#
# make_reference_dngs.sh  —  Regenerate the native reference DNGs used to
# verify the WebAssembly build.
#
# The reference DNGs are large (85-415 MB each) and are deliberately not
# committed; this script rebuilds them from the fixtures in sample/ using the
# native `hiraco-cli`, so a fresh checkout can reproduce the baseline that
# `scripts/compare_dng.py` compares against.
#
# Usage:
#   ./scripts/make_reference_dngs.sh [native-hiraco-cli] [output-dir]
#
# Defaults:
#   native-hiraco-cli  ./build-native-check/hiraco-cli
#   output-dir         build/reference
#
# The compression mode used is Deflate: it is the cheapest mode that still
# exercises the tiled layout and the DNG SDK's Deflate writer.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cli="${1:-${repo_root}/build-native-check/hiraco-cli}"
out_dir="${2:-${repo_root}/build/reference}"
mode="deflate"

fixtures=(
  "_8142492.ORF"
  "_8142490.ORI"
  "P8132479.ORF"
)

if [[ ! -x "${cli}" ]]; then
  echo "error: native hiraco-cli not found or not executable: ${cli}" >&2
  echo "hint: build it first, e.g." >&2
  echo "  cmake -S . -B build-native-check -DCMAKE_BUILD_TYPE=Release \\" >&2
  echo "        -DHIRACO_BUILD_GUI=OFF -DHIRACO_BUILD_TESTS=OFF" >&2
  echo "  cmake --build build-native-check --target hiraco-cli" >&2
  exit 2
fi

mkdir -p "${out_dir}"

for fixture in "${fixtures[@]}"; do
  source_path="${repo_root}/sample/${fixture}"
  if [[ ! -f "${source_path}" ]]; then
    echo "error: missing fixture: ${source_path}" >&2
    exit 2
  fi
  target_path="${out_dir}/${fixture%.*}.dng"
  echo "=== ${fixture} -> ${target_path}"
  "${cli}" convert "${source_path}" "${target_path}" --compression "${mode}"
done

echo
echo "Reference DNGs written to ${out_dir}"
echo "Compare a candidate against them with:"
echo "  .venv/bin/python scripts/compare_dng.py <candidate.dng> ${out_dir}/<name>.dng"
