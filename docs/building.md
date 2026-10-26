# Building

| Preset | Builds | Where |
|---|---|---|
| `linux-debug` | server, client, probe; validation layers on | desktop |
| `linux-release` | the same, optimised | desktop |
| `linux-asan` | debug + AddressSanitizer | desktop |
| `jetson-native-release` | server, probe; `ROBV_ENCODER=jetson` | **on** the Jetson |
| `android-arm64` | the client APK, without Gradle | desktop |

- Dependencies are fetched by CPM into `.cpm-cache/` (once per checkout, not per preset).
- SDL is pinned to `third_party/SDL/` because the APK build compiles its Java from a fixed path.
- Both directories are gitignored.
- `-DROBV_USE_SYSTEM_DEPS=ON` uses `find_package` instead.

## Linux desktop

Requirements:
- CMake ≥ 3.25, Ninja, GCC ≥ 13 or Clang ≥ 17 (C++20 with `<format>`)
- A Vulkan driver and `slangc`. The Vulkan SDK ships `slangc`; otherwise configure downloads a pinned Slang release.
- `vulkan-validation-layers` is optional; debug builds use it if installed.
- ffmpeg 7 or newer with development files (libavcodec, libavutil) for the desktop encoder and decoder. Configure with `-DROBV_WITH_FFMPEG=OFF` to build without them.
- Nothing links `libvulkan.so`; volk opens it at runtime.

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug

./build/linux-debug/tools/robv-probe/robv-probe
./build/linux-debug/server/robv-server --source synthetic:// --encoder null --transport null    # Ctrl-C to stop
./build/linux-debug/client/robv-client
```

- `ROBV_VK_DEVICE=<part of the name>` picks a GPU on multi-GPU machines.
- `linux-asan`: the NVIDIA driver leaks a D-Bus reply on load. Suppress with `LSAN_OPTIONS=suppressions=tools/sanitizers/lsan.supp`.
- Server sources: `synthetic://sphere?points=200000&rate=10`, `mcap:///path/run.mcap?topic=/livox/lidar`, `socket://0.0.0.0:9500` (for the ROS 2 bridge). `robv-server --help` lists the rest.

## Android (Galaxy Tab A11+)

Only the SDK command line tools are needed: no Android Studio, Gradle or AGP.

```sh
sdkmanager "platform-tools" "build-tools;36.1.0" "platforms;android-36" "ndk;29.0.14206865"
export ANDROID_HOME=/opt/android-sdk                       # wherever the SDK lives
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/29.0.14206865
```

Also needed: `javac`, `jar`, `keytool`, `java` (JDK 17+), `zip`, host `slangc`.

```sh
cmake --preset android-arm64
cmake --build --preset android-arm64 --target apk          # -> build/android-arm64/robv-client.apk
cmake --build --preset android-arm64 --target install-apk  # adb install -r
cmake --build --preset android-arm64 --target run-apk      # adb shell am start
adb logcat -s robv-client SDL                              # client log tag: "robv-client"
```

- First build generates `client/android/debug.keystore` (gitignored).
- C++-only changes relink `libmain.so` and repackage; `javac` and `d8` are skipped.
- Target: arm64-v8a only, `minSdk 29`, `targetSdk 36`, 16 KB aligned native libs.
- Vulkan 1.2 is checked at runtime; without it the app shows a message box instead of crashing.

Check the hardware decoder and zero-copy path once on the tablet:

```sh
adb shell "cat /vendor/etc/media_codecs*.xml" | grep -iE 'avc|hevc|low-latency'   # expect c2.mtk.avc.decoder
adb shell cmd gpu vkjson | head -50
```

The client panel also shows whether `VK_ANDROID_external_memory_android_hardware_buffer` is available. The client logs a stream summary every 10 s (`adb logcat -s robv-client`): frames shown, render to decode latency, display queue, packets lost.

The machine running `robv-server` must accept TCP on the control port (9400), e.g. `sudo ufw allow 9400/tcp`. Video goes out over UDP, so it needs no inbound rule. A client on the same machine uses loopback and is not affected.

## Jetson Orin NX (JetPack 6)

- Build natively on the Jetson. Cross compiling needs an NVIDIA sysroot; `cmake/toolchains/aarch64-linux-jetson.cmake` is a documented stub.
- JetPack 6 ships CMake 3.22, so install a current one from Kitware's APT repo:

```sh
sudo apt-get install -y ca-certificates gpg wget
wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor | \
    sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg > /dev/null
echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main" | \
    sudo tee /etc/apt/sources.list.d/kitware.list
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++-13 nvidia-l4t-jetson-multimedia-api
```

```sh
CC=gcc-13 CXX=g++-13 cmake --preset jetson-native-release
cmake --build --preset jetson-native-release
```

**Run the probe over SSH with no `DISPLAY` before anything else:**

```sh
ssh jetson 'unset DISPLAY WAYLAND_DISPLAY; ~/rob-viewer/build/jetson-native-release/tools/robv-probe/robv-probe'
```

It answers two open questions:
1. **Headless Vulkan:** a device must come up with no display (`Headless device ... created yes`). If not, fall back to a headless X/Wayland session or `VK_EXT_headless_surface`.
2. **Zero copy to the encoder:** needs `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`, `/dev/v4l2-nvenc` and `libnvv4l2.so`. Without the extensions, each frame takes one CPU copy.

## ROS 2 bridge

Colcon package outside the main tree; it compiles `common/` itself. The subscriber currently only counts clouds.

```sh
cd ~/ros2_ws/src && ln -s ~/rob-viewer/bridges/ros2 robv_bridge && cd ..
colcon build --packages-select robv_bridge
ros2 run robv_bridge robv_bridge --ros-args -p topic:=/livox/lidar -p server:=127.0.0.1:9500
```

Run `robv-server --source socket://0.0.0.0:9500` on the other end.

## Formatting and linting

```sh
git ls-files -- '*.cpp' '*.h' | xargs clang-format -i      # clang-format 23
clang-tidy -p build/linux-debug server/src/session.cpp     # naming and bug checks from .clang-tidy
```
