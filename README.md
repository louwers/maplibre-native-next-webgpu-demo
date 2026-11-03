# maplibre-native-next WebGPU demo

OpenFreeMap's [Liberty](https://openfreemap.org/) style, rendered in the browser
by [maplibre-native-next](https://github.com/louwers/maplibre-native-next), a
Rust port of the MapLibre Native core, on WebGPU. The page uses
[`@bartlwrs/mln-next-web`](https://github.com/louwers/maplibre-native-next/tree/main/platform/web),
which puts the port behind a trimmed-down MapLibre GL JS API.

Live: https://maplibre-native-next-webgpu-demo.bart-louwers.workers.dev

Drag to pan, scroll or double-click to zoom, right-drag to rotate and pitch.

## Browser support

The page needs WebGPU. It has been tested in Chromium (Chrome and Edge 113
and newer). On Linux, Chromium renders WebGPU through Vulkan, which it leaves
off by default: enable `chrome://flags/#enable-vulkan` (or start it with
`--enable-features=Vulkan`) and restart the browser.

## How it works

- `web/index.html` creates the map, as with GL JS:
  `new Map({container, style, center, zoom})`.
- `web/_headers` makes the page cross-origin isolated
  (`Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` on
  Cloudflare Workers). The package then runs the map in a worker that owns the
  canvas, with tile parsing and layout on more workers. Without the headers,
  it would run everything on the page's thread.
- The package is the port compiled to WebAssembly with wasm-bindgen, drawing
  straight into the canvas with wgpu's WebGPU backend. No Emscripten.

## Build and deploy

```sh
./build.sh            # puts the package from npm into web/vendor/
npx wrangler dev      # http://127.0.0.1:8787
npx wrangler deploy
```

`./build.sh path/to/maplibre-native-next/platform/web/dist` uses a local
build of the package instead (`platform/web/build.sh` in maplibre-native-next
makes one). The package starts its workers and loads its WebAssembly from its
own directory, so it is served from the page's origin, not from a CDN.

`tools/screenshot.mjs` opens the page (a local `web/` directory or a URL) in
headless Chromium and screenshots it.
