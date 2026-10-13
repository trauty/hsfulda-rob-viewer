# Wire protocol

Two channels:
- **Control:** TCP stream of length-prefixed frames (`common/include/robv/common/protocol.h`). The ROS 2 bridge uses the same framing to feed `socket://`.
- **Video:** H.264 (or HEVC) over RTP/UDP (RFC 6184 / RFC 7798), out of band. `welcome` gives codec, size, RTP port and SSRC.

Control is plain TCP framing, not WebSocket: browser clients are ruled out (`list.md`), so RFC 6455 would only add a handshake and a dependency. The control server is a backend in `server/include/robv/server/transport.h`, so WebSocket could be added later.

## Framing

All integers little endian.

```
frame    u32 length            bytes after this field: 2 + payload size
         u8  version           protocol_version, currently 1
         u8  type              msg_type_e
         ... payload

string   u16 length, bytes     UTF-8, no terminator
blob     u32 length, bytes
f32      IEEE 754 bits as a u32
bool     u8, 0 or 1 (anything else is malformed)
```

- The reader checks `length` before waiting for the body. Above `max_frame_size` (64 MiB) the connection is closed.
- Payloads are read exactly: a short payload and trailing bytes are both errors.
- **To grow a message, bump the version.**

## Messages

| Type | Dir | Payload |
|---|---|---|
| 1 `hello` | C→S | `u8 protocol_version` · `string client_name` |
| 2 `input_intent` | C→S | `u32 seq` · `u8 kind` · `f32 dx` · `f32 dy` · `f32 scalar` |
| 3 `ping` | C→S | `u64 t_client_ns` |
| 16 `welcome` | S→C | `u32 client_id` · `u8 protocol_version` · `u8 codec` · `u16 width` · `u16 height` · `u16 rtp_port` · `u32 ssrc` |
| 17 `state_snapshot` | S→C | the `view_state_t`, see below |
| 18 `source_info` | S→C | `u16 n` · n × `string topic` · `u16 m` · m × field · `u64 point_count` · `string source_uri` |
| 19 `pong` | S→C | `u64 t_client_ns` · `u64 t_server_ns` |
| 32 `cloud_frame` | bridge→S | `u64 stamp_ns` · `string frame_id` · `u32 width` · `u32 height` · `u32 point_stride` · `u32 row_stride` · `bool is_dense` · `bool little_endian` · `u16 m` · m × field · `blob data` |

- A field is `string name` · `u32 offset` · `u8 type` · `u32 count`. `type` uses `sensor_msgs/msg/PointField` values (1 int8 … 8 float64), so the bridge copies it through.
- `cloud_frame` is rejected unless `row_stride × height == data size` and `point_stride × width <= row_stride`.

### state_snapshot

```
u64 tick
camera     vec3 target (3 × f32) · f32 yaw · f32 pitch · f32 distance · f32 fov_y · f32 near · f32 far
render     u8 color_mode · f32 point_size · f32 intensity_min · f32 intensity_max · bool grid · bool axes · bool edl
playback   bool playing · f32 rate · u64 cursor_ns · u64 begin_ns · u64 end_ns
string active_topic
```

97 bytes plus the topic name. Serialized once per broadcast, same bytes to every client.

## Intents

Units are device independent: the client converts pixels, so a phone and a tablet dragging the same fraction of their screens move the camera equally.

| Kind | Fields |
|---|---|
| 1 `orbit` | `dx` `dy` radians (yaw, pitch; pitch clamped short of ±90°) |
| 2 `pan` | `dx` `dy` as fractions of the view height at the target distance |
| 3 `zoom` | `scalar` log factor: `distance *= exp(-scalar)`, clamped to ±4 per intent |
| 4 `reset_camera` | none |
| 16 `set_color_mode` | `scalar` 0 intensity, 1 height, 2 rgb, 3 flat |
| 17 `set_point_size` | `scalar` 1 … 32 |
| 18 `set_intensity_range` | `dx` min, `dy` max (min < max) |
| 19–21 `set_grid` `set_axes` `set_eye_dome_lighting` | `scalar != 0` |
| 32 `play`, 33 `pause` | none |
| 34 `set_rate` | `scalar` 0.05 … 16 |
| 35 `seek` | `scalar` fraction 0 … 1 of the playback range |

Intents with a non-finite value or invalid argument are ignored and don't bump the tick.

## A session

```
C: hello            "Galaxy Tab A11+"
S: welcome          client 3, h264 1280x720, rtp 9402, ssrc 0x5e1f...
S: source_info      ["/livox/lidar"], fields x y z intensity, 240000 points
S: state_snapshot   tick 812                                      (at once, then whenever tick changed, ≤ 30 Hz)
   (video: the next frame is an IDR with SPS/PPS in front, for the new client)
C: input_intent     orbit dx 0.012
C: ping             t_client
S: pong             t_client, t_server
S: state_snapshot   tick 813
```
