#!/usr/bin/env bash
# Builds web/demo.js and web/demo.wasm: src/main.c linked against a browser
# build of maplibre-native-ffi on maplibre-native-next.
#
# Usage: ./build.sh [extracted maplibre-native-c-emscripten-wasm32-webgpu directory]
#
# Without an argument it downloads the archive from the $FFI_RELEASE release of
# louwers/maplibre-native-ffi. emcc must be the Emscripten that built the
# archive, because the Rust code in it calls the emdawnwebgpu port that
# Emscripten version pins: Emscripten 6.0.9 from nixpkgs ea9e3ef, which
# `nix shell github:NixOS/nixpkgs/ea9e3ef341e11c193537f674f821a99f9f436729#emscripten`
# provides.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

release="${FFI_RELEASE:-maplibre-native-next-v0.0.3}"
name=maplibre-native-c-emscripten-wasm32-webgpu
ffi="${1:-}"
if [[ -z "$ffi" ]]; then
  ffi="build/$name"
  if [[ ! -d "$ffi" ]]; then
    mkdir -p build
    curl -sSfL -o "build/$name.tar.gz" \
      "https://github.com/louwers/maplibre-native-ffi/releases/download/$release/$name.tar.gz"
    tar -xzf "build/$name.tar.gz" -C build
  fi
fi

# A writable cache for Emscripten's system libraries and ports; the Nix
# package's own is read-only.
export EM_CACHE="${EM_CACHE:-$PWD/build/em-cache}"

mapfile -t link_flags < "$ffi/share/maplibre-native-c/emscripten-link-flags.txt"
emcc src/main.c -O2 -DMLN_STATIC -I"$ffi/include" \
  "$ffi/lib/libmaplibre-native-c.a" "${link_flags[@]}" \
  -sPROXY_TO_PTHREAD -sENVIRONMENT=web,worker \
  -sOFFSCREENCANVAS_SUPPORT=1 "-sOFFSCREENCANVASES_TO_PTHREAD=#map" \
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB \
  -sEXPORTED_FUNCTIONS=_main,_demo_pan,_demo_zoom,_demo_resize \
  -o web/demo.js
