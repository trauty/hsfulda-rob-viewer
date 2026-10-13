#pragma once

#include "robv/common/types.h"

// vsync grid, so frames start at a fixed offset after a vsync instead of drifting
// a drifting start crosses SurfaceFlinger's latch deadline every few dozen frames, showing a frame twice
// Android: AChoreographer callbacks on their own thread, elsewhere start() returns false
// times are CLOCK_MONOTONIC nanoseconds as Choreographer reports, not SDL ticks
namespace robv
{
    struct vsync_t
    { void* backend = nullptr; };
} // namespace robv

namespace robv::vsync
{
    // refresh_hz seeds the period until callbacks have measured it
    bool start(vsync_t& vsync, f32 refresh_hz);
    void stop(vsync_t& vsync);
    bool is_running(const vsync_t& vsync);

    // callbacks only run while wanted, this extends them by 250 ms
    void keep_alive(vsync_t& vsync);

    // latest point offset_ns after a vsync not later than t_ns, 0 before the first vsync
    u64 slot_at(const vsync_t& vsync, u64 t_ns, u64 offset_ns);
    u64 period_ns(const vsync_t& vsync);

    u64 now_ns();
    void sleep_until(u64 t_ns);
} // namespace robv::vsync
