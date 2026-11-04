// OpenFreeMap's Liberty style rendered by maplibre-native-next, the Rust port
// of MapLibre Native, through maplibre-native-ffi's C API, on WebGPU in the
// browser.
//
// main() runs on a pthread (-sPROXY_TO_PTHREAD), where blocking is allowed,
// and owns the page's canvas (-sOFFSCREENCANVASES_TO_PTHREAD=#map). It creates
// the WebGPU device and a surface on the canvas, and attaches a continuous
// map's render session to them with MLN_RENDER_DRIVER_CALLER_GRAPHICS_THREAD:
// the map renders on this thread, straight into the canvas, whenever the loop
// services the session. The session also sizes the canvas: configuring the
// surface sets it, in the same task as the frame drawn into it, so a resize
// never shows a blank canvas. The page reports pointer and wheel input through
// demo_pan() and demo_zoom(), which only record it; the loop turns it into
// camera jumps.

#include <maplibre_native_c.h>

#include <emscripten.h>
#include <emscripten/threading.h>
#include <webgpu/webgpu.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STYLE_URL "https://tiles.openfreemap.org/styles/liberty"
#define TILE_SIZE 512.0
#define MAX_ZOOM 19.0

// MARK: - Errors

static void report(const char* message) {
  fprintf(stderr, "%s\n", message);
  MAIN_THREAD_EM_ASM({ Module.showStatus(UTF8ToString($0), true); }, message);
}

static void check(mln_status status, const mln_diagnostic* diagnostic, const char* what) {
  if (status == MLN_STATUS_OK) return;
  char message[512];
  snprintf(message, sizeof(message), "%s failed (%d): %s", what, (int)status,
           diagnostic != NULL ? diagnostic->message : "");
  report(message);
  exit(1);
}

// Calls `function` with `...` and a diagnostic, and stops on failure.
#define CALL(function, ...)                                        \
  do {                                                             \
    mln_diagnostic diagnostic_ = {.size = sizeof(mln_diagnostic)}; \
    check(function(__VA_ARGS__, &diagnostic_), &diagnostic_, #function); \
  } while (0)

// MARK: - Completions

typedef struct waiter {
  atomic_int done;
  atomic_int released;
  mln_status status;
  char diagnostic[256];
} waiter;

static void on_complete(void* user_data, const mln_completion_result* result) {
  waiter* w = user_data;
  w->status = result->status;
  snprintf(w->diagnostic, sizeof(w->diagnostic), "%.*s", (int)result->diagnostic.size,
           (const char*)result->diagnostic.data);
  atomic_store(&w->done, 1);
}

static void on_release(void* user_data) {
  atomic_store(&((waiter*)user_data)->released, 1);
}

static mln_completion completion_for(waiter* w) {
  memset(w, 0, sizeof(*w));
  return (mln_completion){
    .size = sizeof(mln_completion),
    .callback = on_complete,
    .user_data = w,
    .release_user_data = on_release,
  };
}

// The session, once attached: waits service it, since its graphics work runs
// on this thread.
static mln_render_session session = MLN_HANDLE_NULL;

static void service(void) {
  if (session != MLN_HANDLE_NULL) CALL(mln_render_session_service_driver_work, session);
}

// Sleeps by suspending (JSPI), which lets the browser present the canvas and
// run this thread's events.
static void yield(unsigned milliseconds) { emscripten_sleep(milliseconds); }

static void wait_for(waiter* w, const char* what) {
  while (!atomic_load(&w->done) || !atomic_load(&w->released)) {
    service();
    yield(1);
  }
  if (w->status != MLN_STATUS_OK) {
    char message[512];
    snprintf(message, sizeof(message), "%s failed (%d): %s", what, (int)w->status, w->diagnostic);
    report(message);
    exit(1);
  }
}

// MARK: - Input from the page

// The page's thread adds to these; the render loop takes them.
static _Atomic double pan_x, pan_y, zoom_delta;
static _Atomic double zoom_anchor_x, zoom_anchor_y;
static atomic_uint resize_width, resize_height;

static void add(_Atomic double* value, double delta) {
  double current = atomic_load(value);
  while (!atomic_compare_exchange_weak(value, &current, current + delta)) {
  }
}

EMSCRIPTEN_KEEPALIVE void demo_pan(double dx, double dy) {
  add(&pan_x, dx);
  add(&pan_y, dy);
}

EMSCRIPTEN_KEEPALIVE void demo_zoom(double delta, double x, double y) {
  atomic_store(&zoom_anchor_x, x);
  atomic_store(&zoom_anchor_y, y);
  add(&zoom_delta, delta);
}

EMSCRIPTEN_KEEPALIVE void demo_resize(uint32_t width, uint32_t height) {
  atomic_store(&resize_width, width);
  atomic_store(&resize_height, height);
}

// MARK: - Camera

// Web Mercator world coordinates in logical pixels at a zoom's world size.
static double x_of(double longitude, double world) { return (longitude + 180.0) / 360.0 * world; }
static double y_of(double latitude, double world) {
  const double s = sin(latitude * M_PI / 180.0);
  return (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * world;
}
static double longitude_of(double x, double world) { return x / world * 360.0 - 180.0; }
static double latitude_of(double y, double world) {
  const double n = M_PI - 2.0 * M_PI * y / world;
  return 180.0 / M_PI * atan(sinh(n));
}

typedef struct view {
  double latitude, longitude, zoom;
  uint32_t width, height;
} view;

// Applies pending input to `v`; returns whether it changed.
static bool take_input(view* v) {
  const double dx = atomic_exchange(&pan_x, 0.0);
  const double dy = atomic_exchange(&pan_y, 0.0);
  const double dz = atomic_exchange(&zoom_delta, 0.0);
  if (dx == 0.0 && dy == 0.0 && dz == 0.0) return false;
  double world = TILE_SIZE * pow(2.0, v->zoom);
  double x = x_of(v->longitude, world) - dx;
  double y = y_of(v->latitude, world) - dy;
  if (dz != 0.0) {
    // Keep the point under the cursor in place.
    const double ax = atomic_load(&zoom_anchor_x) - v->width / 2.0;
    const double ay = atomic_load(&zoom_anchor_y) - v->height / 2.0;
    const double zoom = fmin(fmax(v->zoom + dz, 0.0), MAX_ZOOM);
    const double scale = pow(2.0, zoom - v->zoom);
    x = (x + ax) * scale - ax;
    y = (y + ay) * scale - ay;
    v->zoom = zoom;
    world = TILE_SIZE * pow(2.0, v->zoom);
  }
  x = fmod(fmod(x, world) + world, world);
  y = fmin(fmax(y, 0.0), world);
  v->longitude = longitude_of(x, world);
  v->latitude = latitude_of(y, world);
  return true;
}

static void jump(mln_map map, const view* v) {
  const mln_camera_update update = {
    .size = sizeof(update),
    .mode = MLN_CAMERA_UPDATE_MODE_JUMP,
    .camera = {
      .size = sizeof(mln_camera_options),
      .fields = MLN_CAMERA_OPTION_CENTER | MLN_CAMERA_OPTION_ZOOM,
      .center = {.latitude = v->latitude, .longitude = v->longitude},
      .zoom = v->zoom,
    },
  };
  CALL(mln_map_update_camera, map, &update, NULL);
}

// MARK: - WebGPU

typedef struct gpu {
  WGPUInstance instance;
  WGPUAdapter adapter;
  WGPUDevice device;
  WGPUSurface surface;
  WGPUTextureFormat format;
} gpu;

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message,
                       void* out, void* unused) {
  (void)unused;
  if (status != WGPURequestAdapterStatus_Success) fprintf(stderr, "requestAdapter: %.*s\n", (int)message.length, message.data);
  *(WGPUAdapter*)out = adapter;
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* out,
                      void* unused) {
  (void)unused;
  if (status != WGPURequestDeviceStatus_Success) fprintf(stderr, "requestDevice: %.*s\n", (int)message.length, message.data);
  *(WGPUDevice*)out = device;
}

static void on_uncaptured_error(const WGPUDevice* device, WGPUErrorType type, WGPUStringView message, void* a,
                                void* b) {
  (void)device, (void)a, (void)b;
  fprintf(stderr, "WebGPU error %d: %.*s\n", (int)type, (int)message.length, message.data);
}

static void wait_future(WGPUInstance instance, WGPUFuture future) {
  WGPUFutureWaitInfo info = {.future = future};
  wgpuInstanceWaitAny(instance, 1, &info, UINT64_MAX);
}

// The device, and a surface on the canvas this thread owns.
static gpu create_gpu(void) {
  gpu g = {0};
  const WGPUInstanceFeatureName features[] = {WGPUInstanceFeatureName_TimedWaitAny};
  const WGPUInstanceDescriptor instance_descriptor = {.requiredFeatureCount = 1, .requiredFeatures = features};
  g.instance = wgpuCreateInstance(&instance_descriptor);
  if (g.instance == NULL) report("WebGPU: no instance"), exit(1);
  wait_future(g.instance, wgpuInstanceRequestAdapter(g.instance, &(WGPURequestAdapterOptions){.powerPreference = WGPUPowerPreference_HighPerformance},
                                                     (WGPURequestAdapterCallbackInfo){.mode = WGPUCallbackMode_WaitAnyOnly, .callback = on_adapter, .userdata1 = &g.adapter}));
  if (g.adapter == NULL) report("WebGPU found no GPU adapter."), exit(1);
  const WGPUDeviceDescriptor device_descriptor = {
    .uncapturedErrorCallbackInfo = {.callback = on_uncaptured_error},
  };
  wait_future(g.instance, wgpuAdapterRequestDevice(g.adapter, &device_descriptor,
                                                   (WGPURequestDeviceCallbackInfo){.mode = WGPUCallbackMode_WaitAnyOnly, .callback = on_device, .userdata1 = &g.device}));
  if (g.device == NULL) report("WebGPU: no device"), exit(1);
  WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = {
    .chain = {.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector},
    .selector = {.data = "#map", .length = WGPU_STRLEN},
  };
  g.surface = wgpuInstanceCreateSurface(g.instance, &(WGPUSurfaceDescriptor){.nextInChain = &canvas.chain});
  if (g.surface == NULL) report("WebGPU: no surface on the canvas"), exit(1);
  WGPUSurfaceCapabilities capabilities = {0};
  wgpuSurfaceGetCapabilities(g.surface, g.adapter, &capabilities);
  g.format = capabilities.formatCount > 0 ? capabilities.formats[0] : WGPUTextureFormat_BGRA8Unorm;
  wgpuSurfaceCapabilitiesFreeMembers(capabilities);
  return g;
}

// Suspends (JSPI) until the browser's next animation frame on this worker.
EM_ASYNC_JS(void, wait_animation_frame, (), { await new Promise((resolve) => requestAnimationFrame(resolve)); });

// Demands a frame and waits for its result.
static mln_render_result demand(mln_render_session session, uint64_t token) {
  const mln_frame_demand frame_demand = {.size = sizeof(frame_demand), .token = token};
  CALL(mln_render_session_demand_frame, session, &frame_demand);
  for (;;) {
    mln_render_frame_batch batch = MLN_HANDLE_NULL;
    mln_render_frame_batch_view batch_view = {.size = sizeof(batch_view)};
    CALL(mln_render_session_drain_frame_results, session, &batch, &batch_view);
    if (batch_view.result_count > 0) {
      const mln_render_frame_result result =
        *MLN_STRIDED_AT(mln_render_frame_result, batch_view.results, batch_view.result_size,
                        batch_view.result_count - 1);
      mln_render_frame_batch_release(batch);
      if (result.disposition == MLN_RENDER_RESULT_FAILED) {
        char message[256];
        mln_render_session_get_failure(session, message, sizeof(message), &(size_t){0}, NULL);
        report(message);
      }
      return result.disposition;
    }
    service();
    yield(0);
  }
}

int main(int argc, char** argv) {
  view v = {.latitude = 52.3676, .longitude = 4.9041, .zoom = 12.5, .width = 800, .height = 600};
  double scale_factor = 1.0;
  if (argc >= 4) {
    v.width = (uint32_t)atoi(argv[1]);
    v.height = (uint32_t)atoi(argv[2]);
    scale_factor = atof(argv[3]);
  }

  MAIN_THREAD_EM_ASM({ Module.showStatus("Starting the map…", false); });
  mln_runtime runtime = MLN_HANDLE_NULL;
  const mln_runtime_options runtime_options = {.size = sizeof(runtime_options)};
  CALL(mln_runtime_create, &runtime_options, &runtime);

  waiter w;
  mln_completion done = completion_for(&w);
  const mln_map_options map_options = {
    .size = sizeof(map_options),
    .initial_extent = {.width = v.width, .height = v.height, .scale_factor = scale_factor},
    .map_mode = MLN_MAP_MODE_CONTINUOUS,
  };
  mln_map map = MLN_HANDLE_NULL;
  CALL(mln_runtime_create_map, runtime, &map_options, &map, &done);
  wait_for(&w, "mln_runtime_create_map");

  done = completion_for(&w);
  CALL(mln_map_set_style_url, map, mln_buffer_view_from_cstr(STYLE_URL), &done);
  wait_for(&w, "mln_map_set_style_url");
  jump(map, &v);

  const gpu g = create_gpu();
  done = completion_for(&w);
  const mln_web_gpu_surface_descriptor descriptor = {
    .size = sizeof(descriptor),
    .logical_size = {.width = v.width, .height = v.height},
    .context = {.instance = g.instance, .device = g.device},
    .surface = g.surface,
    .format = (uint32_t)g.format,
  };
  const mln_render_session_attach_options attach_options = {
    .size = sizeof(attach_options), .driver = MLN_RENDER_DRIVER_CALLER_GRAPHICS_THREAD
  };
  CALL(mln_map_attach_web_gpu_surface, map, &descriptor, &attach_options, &session, &done);
  wait_for(&w, "mln_map_attach_web_gpu_surface");
  MAIN_THREAD_EM_ASM({ Module.showStatus("", false); });

  for (uint64_t token = 1;; token++) {
    const uint32_t width = atomic_exchange(&resize_width, 0);
    const uint32_t height = atomic_exchange(&resize_height, 0);
    if (width != 0 && height != 0 && (width != v.width || height != v.height)) {
      v.width = width;
      v.height = height;
      done = completion_for(&w);
      CALL(mln_render_session_resize, session, (mln_logical_size){width, height}, &done);
      wait_for(&w, "mln_render_session_resize");
    }
    if (take_input(&v)) jump(map, &v);

    // The frame shows when this thread yields. Waiting for the next
    // animation frame paces rendering to the display.
    demand(session, token);
    wait_animation_frame();
  }
}
