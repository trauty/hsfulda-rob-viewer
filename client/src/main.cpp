// shows the server's stream and turns touches into intents
// same code on desktop and on Android (libmain.so loaded by SDLActivity)

#include "robv/client/app.h"
#include "robv/common/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <charconv>
#include <cstdio>
#include <memory>
#include <string_view>

namespace
{
    template <typename int_t>
    bool parse_int(std::string_view text, int_t& out)
    {
        const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), out);
        return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
    }

    bool parse_size(std::string_view text, u32& width, u32& height)
    {
        const size_t x = text.find('x');
        return x != std::string_view::npos && parse_int(text.substr(0, x), width) &&
               parse_int(text.substr(x + 1), height) && width > 0 && height > 0;
    }

    void print_usage()
    {
        std::fprintf(stderr,
                     "usage: robv-client [options]\n"
                     "  --decoder <kind>      null (default); mediacodec and ffmpeg are not implemented yet\n"
                     "  --size <w>x<h>        initial window size, default 1280x720\n"
                     "  --frames <n>          exit after n presented frames, 0 runs until closed (default)\n"
                     "  --continuous          draw every display refresh instead of only when something changed\n"
                     "  --validation          Vulkan validation layers (default in desktop debug builds)\n"
                     "  --no-validation\n"
                     "  --log-level <level>   trace debug info warn error\n");
    }
} // namespace

int main(int argc, char** argv)
{
    robv::app_config_t config;
    // no validation layer on Android
#if defined(ROBV_VALIDATION_DEFAULT) && !defined(ROBV_PLATFORM_ANDROID)
    config.validation = true;
#endif
    u64 max_frames = 0;
    robv::log::level_e log_level = robv::log::level_e::info;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        const std::string_view value = i + 1 < argc ? std::string_view(argv[i + 1]) : std::string_view();

        bool ok = true;
        bool takes_value = true;
        if (arg == "--help" || arg == "-h")
        {
            print_usage();
            return 0;
        }
        else if (arg == "--decoder") { ok = robv::decoder::parse_kind(value, config.decoder); }
        else if (arg == "--size") { ok = parse_size(value, config.width, config.height); }
        else if (arg == "--frames") { ok = parse_int(value, max_frames); }
        else if (arg == "--log-level") { ok = robv::log::parse_level(value, log_level); }
        else if (arg == "--continuous")
        {
            config.continuous = true;
            takes_value = false;
        }
        else if (arg == "--validation" || arg == "--no-validation")
        {
            config.validation = arg == "--validation";
            takes_value = false;
        }
        else
        {
            ok = false;
        }

        if (!ok)
        {
            std::fprintf(stderr, "robv-client: bad argument '%s'\n\n", argv[i]);
            print_usage();
            return 2;
        }
        if (takes_value) { ++i; }
    }

    robv::log::init("robv-client");
    robv::log::set_level(log_level);
    ROBV_LOG_INFO("CLIENT", "robv-client {}", ROBV_VERSION);

    // heap allocated, its parts point into it
    std::unique_ptr<robv::app_t> app = std::make_unique<robv::app_t>();
    if (!robv::app::init(*app, config))
    {
        robv::app::shutdown(*app);
        robv::log::shutdown();
        return 1;
    }

    while (!app->quit)
    {
        SDL_Event event;
        // sleeps until a frame is due, see app::wait
        robv::app::wait(*app);
        while (SDL_PollEvent(&event)) { robv::app::handle_event(*app, event); }

        robv::app::iterate(*app);
        if (max_frames > 0 && app->frames >= max_frames) { break; }
    }

    ROBV_LOG_INFO("CLIENT", "closing after {} frames", app->frames);
    robv::app::shutdown(*app);
    robv::log::shutdown();
    return 0;
}
