#!/usr/bin/env bash
# Builds web/demo.js and web/demo.wasm: src/main.c linked against a browser
# build of maplibre-native-ffi on maplibre-native-next.
#
# Usage: ./build.sh [extracted maplibre-native-c-emscripten-wasm32-webgpu-next directory]
#
# Without an argument it downloads the archive from the $FFI_RELEASE release of
# louwers/maplibre-native-ffi, which maplibre-native-ffi's CI built. emcc must
# be the Emscripten that built the archive, because the Rust code in it calls
# the emdawnwebgpu port that Emscripten version pins: 6.0.13, the version that
# maplibre-native-ffi's .config/mise.emscripten.toml pins, which
# `emsdk install 6.0.13` provides.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

release="${FFI_RELEASE:-maplibre-native-next-v0.0.3-de59db8}"
name=maplibre-native-c-emscripten-wasm32-webgpu-next
ffi="${1:-}"
if [[ -z "$ffi" ]]; then
  # One directory per release, so a new release isn't shadowed by an old one.
  ffi="build/$release/$name"
  if [[ ! -d "$ffi" ]]; then
    mkdir -p "build/$release"
    curl -sSfL -o "build/$release/$name.tar.gz" \
      "https://github.com/louwers/maplibre-native-ffi/releases/download/$release/$name.tar.gz"
    tar -xzf "build/$release/$name.tar.gz" -C "build/$release"
  fi
fi

# A cache for Emscripten's system libraries and ports of the demo's own, which
# a read-only Emscripten install also needs.
export EM_CACHE="${EM_CACHE:-$PWD/build/em-cache}"

mapfile -t link_flags < "$ffi/share/maplibre-native-c/emscripten-link-flags.txt"
emcc src/main.c -O2 -DMLN_STATIC -I"$ffi/include" \
  "$ffi/lib/libmaplibre-native-c.a" "${link_flags[@]}" \
  -sPROXY_TO_PTHREAD -sENVIRONMENT=web,worker \
  -sOFFSCREENCANVAS_SUPPORT=1 "-sOFFSCREENCANVASES_TO_PTHREAD=#map" \
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB \
  -sEXPORTED_FUNCTIONS=_main,_demo_pan,_demo_zoom,_demo_resize \
  -o web/demo.js
