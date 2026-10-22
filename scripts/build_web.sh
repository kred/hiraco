#!/usr/bin/env bash
#
# build_web.sh  —  Build the web application.
#
# The web application runs the whole conversion in the browser, so it needs
# the WebAssembly module next to its own sources.  This script builds that
# module, copies it into the application and builds the application, so the
# usual invocation is
#
#   ./scripts/build_web.sh
#
# and the result is a static site in web/dist that can be served by any
# static host.
#
# The module is built with the `wasm` CMake preset, which needs to know where
# the Emscripten toolchain file lives.  scripts/build_wasm.sh finds it, so
# that path is worked out the same way here.
#
# Set EMSCRIPTEN_TOOLCHAIN_FILE to override the detection.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "${repo_root}"

echo "==> Building the WebAssembly module"
"${repo_root}/scripts/build_wasm.sh" hiraco-web-assets

echo
echo "==> Installing the web application dependencies"
npm --prefix "${repo_root}/web" ci

echo
echo "==> Building the web application"
npm --prefix "${repo_root}/web" run build

echo
echo "Built:"
echo "  web/dist/                        (static site)"
echo "  web/dist/wasm/hiraco-wasm.js      (browser module)"
echo "  web/dist/wasm/hiraco-wasm.wasm    (browser module)"
echo
echo "Try it:"
echo "  npm --prefix web run preview      (serves web/dist with the headers the module needs)"
