# Architecture

Purpose-built remote rendering (see `list.md` for why nothing off the shelf works): the robot renders the cloud with
Vulkan and streams H.264, and a native Android app shows the stream and drives one camera shared by all clients.

```
                       robot: Jetson Orin NX                                      tablet: Galaxy Tab A11+
 ┌──────────────────────────────────────────────────────────────────────┐   ┌────────────────────────────┐
 │                                                                      │   │                            │
 │  source ──cloud──▶ renderer ──NV12──▶ encoder ──H.264──▶ video sink ─┼───┼─▶ decoder ──▶ presenter     │
 │    ▲                  ▲                                  (RTP/UDP,   │   │                  ▲         │
 │    │                  │ view_state                        1 encode,  │   │                  │ ImGui   │
 │  synthetic://         │                                   N sends)   │   │                  │         │
 │  mcap://        session (owns view_state) ◀── intents ── control ◀───┼───┼── touches ──▶ intents     │
 │  socket:// ◀─ ROS 2 bridge             ──── snapshots ──▶ (TCP)  ────┼───┼─▶ connection state         │
 └──────────────────────────────────────────────────────────────────────┘   └────────────────────────────┘
```

## Modules

| Directory | Target | What it is |
|---|---|---|
| `common/` | `robv::common`, `robv::log` | Types, wire protocol, `view_state_t` and its fold. No Vulkan, platform code or dependencies (the ROS 2 bridge compiles it too). Logging is split off because it needs spdlog. |
| `vkcore/` | `robv::vkcore` | Vulkan 1.2 setup shared by both binaries: instance, device, queues, VMA, buffers, images, embedded shaders, timeline semaphores. Headless server vs. presenting client differ by `need_presentation`. |
| `server/` | `robv-server` | Source → renderer → encoder → sinks, driven by `session::tick()` at a fixed rate. |
| `client/` | `robv-client` | SDL3 window, swapchain, ImGui panel, decoder. Executable on Linux, `libmain.so` in an APK on Android. |
| `bridges/ros2/` | `robv_bridge` | Colcon package outside the main tree. Subscribes to `PointCloud2`, forwards to `socket://`. |
| `tools/robv-probe/` | `robv-probe` | Reports what a machine offers robv. Run it first on a new Jetson. |

Code style:
- Procedural and data-oriented: structs (`*_t`), enums (`*_e`), free functions in a namespace per module (`robv::source::open(source_t&, ...)`).
- Backends are an enum and a `switch`, not virtual classes. Each keeps its state behind a `void* backend` that only its `.cpp` interprets.

## The server is authoritative

- Clients send intents (`orbit dx dy`, `zoom scalar`, `set_color_mode`, ...), never state.
- The server folds them into the one `view_state_t` in arrival order (`view_state::apply`), bumps `tick`, and broadcasts snapshots at up to 30 Hz.
- Video is rendered from the same state, so all clients see the same thing.
- All clients have equal rights: concurrent drags add up, discrete settings are last-write-wins. No locking, no ownership.
- `apply` is pure in `(state, intent)`, so a recording of intents replays exactly.

## Renderer contract: NV12 out

- The encoder takes NV12 natively, so the renderer outputs NV12. RGB → NV12 runs on the GPU (`server/shaders/rgb_to_nv12.slang`, BT.709 limited range). The encoder never sees RGB.
- Goal is zero copy: an NV12 dmabuf that is both a Vulkan image and an `NvBufSurface`.
- Whether the Tegra driver can export/import it decides between zero copy and one GPU → CPU → `NvBufSurface` copy per frame. The copy is fine for the first working stream.
- `robv-probe` reports the relevant extensions: `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`.

## Encode once, send N times

- One camera means one render, one encode, one RTP stream (one SSRC), sent to every peer.
- A late joiner triggers `encoder::request_keyframe`, so it decodes from the next frame instead of waiting for a periodic IDR.

## Point clouds reach the GPU unconverted

- `point_cloud_frame_t` mirrors `sensor_msgs/msg/PointCloud2` without ROS: fields, strides, raw blob.
- `point_cloud::resolve_layout` finds x/y/z, intensity (or reflectivity) and rgb by name; `make_gpu_layout` turns them into push constants (`cloud_gpu_layout_t`) for the vertex shader to unpack the blob directly.
- XYZ, XYZI, XYZRGB and custom driver layouts work with no CPU pass and no per-schema code.

## Android without Gradle

- `cmake/RobvAndroidApk.cmake` drives `aapt2`, `javac`, `d8`, `zip`, `zipalign` and `apksigner` directly.
- SDL3's Java needs only `android.jar`, and `org.libsdl.app.SDLActivity` already loads `libSDL3.so` and `libmain.so`. The manifest names SDL's activity, so robv has no Java of its own.
- Native libraries are stored uncompressed and 16 KB aligned (`extractNativeLibs=false`).

## Status

| Piece | State |
|---|---|
| Protocol, `view_state`, cloud layout, URIs | Done |
| vkcore (headless and presenting) | Done |
| `synthetic://` source | Done: Fibonacci sphere, XYZI, configurable rate |
| `socket://` source | Done: accepts `cloud_frame` from the bridge |
| `mcap://` source | Reads the summary (PointCloud2 topics, time range). No playback. |
| Renderer | `vulkan`: points straight from the blob (any float32 XYZ layout, padded rows, intensity of any field type, packed rgb), colour by intensity, height, rgb or flat, point size, grid and axes; `rgb_to_nv12` on the GPU, one readback per frame (0.4 ms at 200k points, 3 ms at 10M on an RTX 4070). No eye-dome lighting yet. `null`: colour bars driven by the view state. `--dump` writes the last frame as NV12. |
| Encoder | `ffmpeg` (desktop): NVENC, falling back to x264, low latency, IDR on request (0.9 ms per 720p frame on an RTX 4070). `null` counts frames. `jetson` opens and checks `/dev/v4l2-nvenc`, then refuses. |
| Transport | `null`: simulated client that orbits, zooms, pings. `net`: TCP control server (any number of clients, slow readers dropped) and RTP video, H.264 packetized once and sent to every client that punched, IDR when one joins or loses packets. |
| Client | Swapchain, video drawn letterboxed through a YCbCr sampler (NV12 uploads on the desktop, zero copy on Android), ImGui panel, Android surface loss/restore. Control channel on its own thread (connect, reconnect with backoff, ping/RTT); drag orbits, two fingers or right drag pan, pinch or wheel zooms; panel edits render settings. |
| Decoder | `mediacodec` (Android, the default there): `c2.mtk.avc.decoder` in async mode into an `AImageReader` of GPU buffers, each `AHardwareBuffer` imported into Vulkan with the driver's external format (MediaTek outputs a tiled format a CPU copy cannot read). On the Tab A11+ over Wi-Fi: 30 fps, render to decoded 24 ms idle and 34 ms dragging, plus 34 to 44 ms in the display queue. `ffmpeg` (desktop default): software H.264, concealed frames shown after a loss. `null`: test pattern. |
| ROS 2 bridge | Socket framing done. Subscriber is a stub that only counts clouds. |
