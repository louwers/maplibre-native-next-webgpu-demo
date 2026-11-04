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
newer, or Safari 27 or newer.

On Linux, Chromium renders WebGPU through Vulkan, which it leaves off by
default. Enable `chrome://flags/#enable-vulkan` (or start it with
`--enable-features=Vulkan`) and restart the browser.

## How it works

- `src/main.c` uses the C API. It runs on a pthread (`-sPROXY_TO_PTHREAD`)
  that owns the page's canvas (`-sOFFSCREENCANVASES_TO_PTHREAD=#map`). It
  creates the WebGPU device and a surface on the canvas, and attaches a
  continuous map's render session to them with
  `MLN_RENDER_DRIVER_CALLER_GRAPHICS_THREAD`. The map then renders on that
  thread, straight into the canvas, whenever the loop services the session.
  The loop waits for the worker's `requestAnimationFrame` between frames, which
  paces rendering to the display. Pointer and wheel input become camera jumps.
- maplibre-native-ffi's browser build is an Emscripten module. The Rust port
  reaches WebGPU through a wgpu backend over `webgpu.h` (crate
  `mln_wgpu_webgpu_h`), which Dawn's emdawnwebgpu port implements on the
  browser's WebGPU. Tiles, glyphs and sprites come through `emscripten_fetch`.
- `web/_headers` sets `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp` on Cloudflare Workers. The map
  fetches its tiles with CORS, which `require-corp` allows.

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
