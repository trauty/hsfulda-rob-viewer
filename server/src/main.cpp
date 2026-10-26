// renders the shared view of a point cloud source and streams it to every client

#include "robv/common/log.h"
#include "robv/server/session.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace
{
    std::atomic<bool> quit_requested{false};

    void on_signal(int) { quit_requested.store(true); }

    void print_usage()
    {
        std::fprintf(stderr,
                     "usage: robv-server [options]\n"
                     "  --source <uri>        synthetic://sphere?points=200000&rate=10 (default)\n"
                     "                        mcap:///path/run.mcap?topic=/livox/lidar\n"
                     "                        socket://0.0.0.0:9500\n"
                     "  --renderer <kind>     null (default), vulkan\n"
                     "  --encoder <kind>      null (default), ffmpeg (NVENC or x264), jetson\n"
                     "  --transport <kind>    null (default), net\n"
                     "  --codec <codec>       h264 (default), hevc\n"
                     "  --size <w>x<h>        encode resolution, default 1280x720\n"
                     "  --fps <n>             default 30\n"
                     "  --bitrate <kbit/s>    default 8000\n"
                     "  --port <n>            control port, default 9400 (RTP goes from port + 2)\n"
                     "  --frames <n>          exit after n frames, 0 runs until SIGINT (default)\n"
                     "  --dump <file>         write the last frame as raw NV12 on exit, view it with\n"
                     "                        ffplay -f rawvideo -pixel_format nv12 -video_size <w>x<h> <file>\n"
                     "  --validation          Vulkan validation layers (default in debug builds)\n"
                     "  --no-validation\n"
                     "  --log-level <level>   trace debug info warn error\n");
    }

    template <typename int_t>
    bool parse_int(std::string_view text, int_t& out)
    {
        const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), out);
        return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
    }

    bool parse_size(std::string_view text, u32& width, u32& height)
    {
        const size_t x = text.find('x');
        if (x == std::string_view::npos) { return false; }
        return parse_int(text.substr(0, x), width) && parse_int(text.substr(x + 1), height) && width >= 16 &&
               height >= 16 && width <= 8192 && height <= 8192;
    }

    struct options_t
    {
        robv::session_config_t session;
        u64 max_frames = 0;
        std::string dump_path;
        robv::log::level_e log_level = robv::log::level_e::info;
    };

    std::string_view next_value(int argc, char** argv, int& i)
    {
        if (i + 1 >= argc) { return {}; }
        return argv[++i];
    }

    // 0 run, 1 bad argument, 2 after help
    int parse_args(int argc, char** argv, options_t& out)
    {
#if defined(ROBV_VALIDATION_DEFAULT)
        out.session.validation = true;
#endif
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            bool ok = true;
            if (arg == "--help" || arg == "-h")
            {
                print_usage();
                return 2;
            }
            else if (arg == "--source")
            {
                out.session.source_uri = next_value(argc, argv, i);
                ok = !out.session.source_uri.empty();
            }
            else if (arg == "--renderer")
            {
                ok = robv::renderer::parse_kind(next_value(argc, argv, i), out.session.renderer);
            }
            else if (arg == "--encoder")
            {
                ok = robv::encoder::parse_kind(next_value(argc, argv, i), out.session.encoder);
            }
            else if (arg == "--transport")
            {
                ok = robv::transport::parse_kind(next_value(argc, argv, i), out.session.transport);
            }
            else if (arg == "--codec")
            {
                const std::string_view codec = next_value(argc, argv, i);
                if (codec == "h264") { out.session.codec = robv::video_codec_e::h264; }
                else if (codec == "hevc") { out.session.codec = robv::video_codec_e::hevc; }
                else
                {
                    ok = false;
                }
            }
            else if (arg == "--size")
            {
                ok = parse_size(next_value(argc, argv, i), out.session.width, out.session.height);
            }
            else if (arg == "--fps")
            {
                ok = parse_int(next_value(argc, argv, i), out.session.fps) && out.session.fps > 0;
            }
            else if (arg == "--bitrate") { ok = parse_int(next_value(argc, argv, i), out.session.bitrate_kbps); }
            else if (arg == "--port")
            {
                ok = parse_int(next_value(argc, argv, i), out.session.control_port) && out.session.control_port < 65534;
                out.session.rtp_port = static_cast<u16>(out.session.control_port + 2);
            }
            else if (arg == "--frames") { ok = parse_int(next_value(argc, argv, i), out.max_frames); }
            else if (arg == "--dump")
            {
                out.dump_path = next_value(argc, argv, i);
                ok = !out.dump_path.empty();
            }
            else if (arg == "--validation") { out.session.validation = true; }
            else if (arg == "--no-validation") { out.session.validation = false; }
            else if (arg == "--log-level") { ok = robv::log::parse_level(next_value(argc, argv, i), out.log_level); }
            else
            {
                ok = false;
            }

            if (!ok)
            {
                std::fprintf(stderr, "robv-server: bad argument '%s'\n\n", argv[i]);
                print_usage();
                return 1;
            }
        }
        return 0;
    }

    void dump_frame(const robv::planar_frame_t& frame, const std::string& path)
    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        const bool ok = file && std::fwrite(frame.data.data(), 1, frame.data.size(), file) == frame.data.size();
        if (file) { std::fclose(file); }
        if (ok) { ROBV_LOG_INFO("SERVER", "last frame written to {}, NV12 {}x{}", path, frame.width, frame.height); }
        else
        {
            ROBV_LOG_ERROR("SERVER", "could not write {}", path);
        }
    }

    u64 steady_ns()
    {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
} // namespace

int main(int argc, char** argv)
{
    options_t options;
    if (const int result = parse_args(argc, argv, options); result != 0) { return result == 2 ? 0 : 2; }

    robv::log::init("robv-server");
    robv::log::set_level(options.log_level);
    ROBV_LOG_INFO("SERVER", "robv-server {}, source {}, renderer {}, encoder {}, transport {}, {}x{} @ {} fps",
                  ROBV_VERSION, options.session.source_uri, robv::renderer::kind_name(options.session.renderer),
                  robv::encoder::kind_name(options.session.encoder),
                  robv::transport::kind_name(options.session.transport), options.session.width, options.session.height,
                  options.session.fps);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::unique_ptr<robv::session_t> session = std::make_unique<robv::session_t>();
    if (!robv::session::init(*session, options.session))
    {
        ROBV_LOG_FATAL("SERVER", "startup failed, see above");
        robv::session::shutdown(*session);
        robv::log::shutdown();
        return 1;
    }

    // fixed rate
    const u64 period_ns = 1'000'000'000ull / options.session.fps;
    u64 next_ns = steady_ns();
    while (!quit_requested.load())
    {
        const u64 now_ns = steady_ns();
        robv::session::tick(*session, now_ns);
        if (options.max_frames > 0 && session->stats.frames >= options.max_frames) { break; }

        next_ns += period_ns;
        const u64 after_ns = steady_ns();
        if (after_ns < next_ns) { std::this_thread::sleep_for(std::chrono::nanoseconds(next_ns - after_ns)); }
        else
        {
            next_ns = after_ns;
        }
    }

    ROBV_LOG_INFO("SERVER", "shutting down after {} frames ({})", session->stats.frames,
                  quit_requested.load() ? "signal" : "frame limit");
    if (!options.dump_path.empty()) { dump_frame(session->frame, options.dump_path); }
    robv::session::shutdown(*session);
    robv::log::shutdown();
    return 0;
}
