# hsfulda-rob-viewer

Remote point cloud viewing for a robot: a Jetson renders ROS 2 / MCAP point clouds with Vulkan and streams
H.264, and a native Android app (Galaxy Tab A11+ baseline) shows the stream and drives one camera that every connected device
shares.

- [docs/architecture.md](docs/architecture.md): modules, data flow, what is real and what is a stub
- [docs/building.md](docs/building.md): desktop, Android without Gradle, Jetson, ROS 2 bridge
- [docs/protocol.md](docs/protocol.md): the control and bridge wire format

```sh
cmake --preset linux-debug && cmake --build --preset linux-debug
./build/linux-debug/server/robv-server --source synthetic:// --encoder null --transport null
./build/linux-debug/client/robv-client

# control over TCP
./build/linux-debug/server/robv-server --transport net
./build/linux-debug/client/robv-client --server 127.0.0.1:9400

# the whole stream: Vulkan points, NVENC or x264, RTP to every client
./build/linux-debug/server/robv-server --renderer vulkan --encoder ffmpeg --transport net
./build/linux-debug/client/robv-client --server 127.0.0.1:9400

# points rendered with Vulkan, the last frame saved as NV12
./build/linux-debug/server/robv-server --renderer vulkan --frames 90 --dump frame.nv12
ffplay -f rawvideo -pixel_format nv12 -video_size 1280x720 frame.nv12
```
