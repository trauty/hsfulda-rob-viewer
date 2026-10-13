#include "robv/client/vsync.h"

#include "robv/common/log.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <ctime>

#if defined(ROBV_PLATFORM_ANDROID)
    #include <android/choreographer.h>
    #include <android/looper.h>
    #include <thread>
#endif

namespace robv::vsync
{
    namespace
    {
#if defined(ROBV_PLATFORM_ANDROID)
        // bridges gaps between touch batches, stops soon after the motion does
        constexpr u64 keep_alive_ns = 250'000'000ull;

        struct choreographer_t
        {
            std::thread thread;
            ALooper* looper = nullptr;
            std::atomic<bool> ready{false};
            std::atomic<bool> quit{false};
            std::atomic<u64> want_until_ns{0};
            std::atomic<u64> last_vsync_ns{0};
            std::atomic<u64> period_ns{0};

            // looper thread only
            AChoreographer* choreographer = nullptr;
            bool posted = false;
        };

        void on_frame(int64_t frame_time_ns, void* data);

        void post(choreographer_t& c)
        {
            if (c.posted || c.quit.load() || c.want_until_ns.load() < now_ns()) { return; }
            c.posted = true;
            AChoreographer_postFrameCallback64(c.choreographer, on_frame, &c);
        }

        // period is measured from callbacks, so it follows refresh rate switches
        // a gap of several periods is an idle or late callback, not a slow display
        void on_frame(int64_t frame_time_ns, void* data)
        {
            choreographer_t& c = *static_cast<choreographer_t*>(data);
            c.posted = false;

            const u64 t = static_cast<u64>(frame_time_ns);
            const u64 last = c.last_vsync_ns.load();
            const u64 period = c.period_ns.load();
            if (last != 0 && t > last && period != 0)
            {
                const f64 cycles = std::max(1.0, std::round(static_cast<f64>(t - last) / static_cast<f64>(period)));
                const f64 measured = static_cast<f64>(t - last) / cycles;
                // within 25 % is jitter, beyond it the display changed rate
                const bool same_rate = std::fabs(measured - static_cast<f64>(period)) < 0.25 * static_cast<f64>(period);
                const f64 next = same_rate ? 0.9 * static_cast<f64>(period) + 0.1 * measured : measured;
                c.period_ns.store(static_cast<u64>(next));
            }
            c.last_vsync_ns.store(t);
            post(c);
        }

        void run(choreographer_t* c)
        {
            ALooper_prepare(0);
            c->looper = ALooper_forThread();
            ALooper_acquire(c->looper);
            c->choreographer = AChoreographer_getInstance();
            c->ready.store(true);

            while (!c->quit.load())
            {
                post(*c);
                ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
            }
        }
#endif
    } // namespace

    bool start(vsync_t& vsync, f32 refresh_hz)
    {
#if defined(ROBV_PLATFORM_ANDROID)
        choreographer_t* c = new choreographer_t;
        c->period_ns.store(static_cast<u64>(1e9 / static_cast<f64>(refresh_hz > 0.0f ? refresh_hz : 60.0f)));
        c->thread = std::thread(run, c);
        while (!c->ready.load()) { std::this_thread::yield(); }
        if (!c->choreographer)
        {
            ROBV_LOG_WARN("VSYNC", "no AChoreographer, frames are paced without vsync");
            vsync.backend = c;
            stop(vsync);
            return false;
        }
        vsync.backend = c;
        ROBV_LOG_INFO("VSYNC", "pacing frames to vsync via AChoreographer");
        return true;
#else
        (void)vsync;
        (void)refresh_hz;
        return false;
#endif
    }

    void stop(vsync_t& vsync)
    {
#if defined(ROBV_PLATFORM_ANDROID)
        choreographer_t* c = static_cast<choreographer_t*>(vsync.backend);
        if (!c) { return; }
        c->quit.store(true);
        if (c->looper) { ALooper_wake(c->looper); }
        if (c->thread.joinable()) { c->thread.join(); }
        if (c->looper) { ALooper_release(c->looper); }
        delete c;
#endif
        vsync.backend = nullptr;
    }

    bool is_running(const vsync_t& vsync) { return vsync.backend != nullptr; }

    void keep_alive(vsync_t& vsync)
    {
#if defined(ROBV_PLATFORM_ANDROID)
        choreographer_t* c = static_cast<choreographer_t*>(vsync.backend);
        if (!c) { return; }
        const u64 now = now_ns();
        // only an idle looper needs waking, a busy one reposts from its callback
        if (c->want_until_ns.exchange(now + keep_alive_ns) < now) { ALooper_wake(c->looper); }
#else
        (void)vsync;
#endif
    }

    u64 slot_at(const vsync_t& vsync, u64 t_ns, u64 offset_ns)
    {
#if defined(ROBV_PLATFORM_ANDROID)
        const choreographer_t* c = static_cast<const choreographer_t*>(vsync.backend);
        if (!c) { return 0; }
        const i64 base = static_cast<i64>(c->last_vsync_ns.load() + offset_ns);
        const i64 period = static_cast<i64>(c->period_ns.load());
        if (base == static_cast<i64>(offset_ns) || period <= 0) { return 0; }
        // floor division, t may lie before the last vsync's slot
        const i64 delta = static_cast<i64>(t_ns) - base;
        const i64 cycles = delta >= 0 ? delta / period : -((-delta + period - 1) / period);
        return static_cast<u64>(base + cycles * period);
#else
        (void)vsync;
        (void)t_ns;
        (void)offset_ns;
        return 0;
#endif
    }

    u64 period_ns(const vsync_t& vsync)
    {
#if defined(ROBV_PLATFORM_ANDROID)
        const choreographer_t* c = static_cast<const choreographer_t*>(vsync.backend);
        return c ? c->period_ns.load() : 0;
#else
        (void)vsync;
        return 0;
#endif
    }

    u64 now_ns()
    {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<u64>(ts.tv_sec) * 1'000'000'000ull + static_cast<u64>(ts.tv_nsec);
    }

    // absolute deadline, oversleeping one frame cannot push the next later
    void sleep_until(u64 t_ns)
    {
        const timespec ts{.tv_sec = static_cast<time_t>(t_ns / 1'000'000'000ull),
                          .tv_nsec = static_cast<long>(t_ns % 1'000'000'000ull)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
    }
} // namespace robv::vsync
