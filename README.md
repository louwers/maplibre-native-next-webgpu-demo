# maplibre-native-next WebGPU demo

OpenFreeMap's [Liberty](https://openfreemap.org/) style, rendered in the browser
by [maplibre-native-next](https://github.com/louwers/maplibre-native-next), a
Rust port of the MapLibre Native core, through the C API of
[maplibre-native-ffi](https://github.com/louwers/maplibre-native-ffi)
(branch `implementation-switch`), with WebGPU.

Live: https://maplibre-native-next-webgpu-demo.bart-louwers.workers.dev

Drag to pan; scroll or double-click to zoom.

## Browser support

The page needs WebGPU with a GPU adapter, cross-origin isolation (for threads)
and WebAssembly JavaScript Promise Integration (JSPI): Chrome or Edge 137 or
newer. Safari has no JSPI yet.

On Linux, Chromium renders WebGPU through Vulkan, which it leaves off by
default. Enable `chrome://flags/#enable-vulkan` (or start it with
`--enable-features=Vulkan`) and restart the browser.

## How it works

- `src/main.c` uses the C API: a continuous map with an offscreen render
  session. It runs on a pthread (`-sPROXY_TO_PTHREAD`), demands frames, reads
  each rendered frame back and hands the pixels to the page, which draws them
  into a 2D canvas. Pointer and wheel input become camera jumps.
- maplibre-native-ffi's browser build is an Emscripten module. The Rust port
  reaches WebGPU through a wgpu backend over `webgpu.h` (crate
  `mln_wgpu_webgpu_h`), which Dawn's emdawnwebgpu port implements on the
  browser's WebGPU. Tiles, glyphs and sprites come through `emscripten_fetch`.
- `web/_headers` sets `Cross-Origin-Opener-Policy` and
  `Cross-Origin-Embedder-Policy` on Cloudflare Workers.

Reading every frame back costs time; rendering straight into the canvas is the
next step.

## Build and deploy

```sh
nix shell github:NixOS/nixpkgs/ea9e3ef341e11c193537f674f821a99f9f436729#emscripten --command ./build.sh [maplibre-native-ffi browser build]
npx wrangler dev      # http://127.0.0.1:8787
npx wrangler deploy
```

`build.sh` links against an installed `maplibre-native-c-emscripten-wasm32-webgpu`
build of maplibre-native-ffi, or downloads the archive from a release of
louwers/maplibre-native-ffi. Use the Emscripten that built that archive
(6.0.9, from the pinned nixpkgs above): the Rust code in it calls the
emdawnwebgpu port that Emscripten version pins.

`tools/screenshot.mjs` opens the page (a local `web/` directory or a URL) in
headless Chromium and screenshots it.
