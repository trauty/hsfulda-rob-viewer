#include "robv/common/log.h"

#include <memory>
#include <spdlog/spdlog.h>

#if defined(ROBV_PLATFORM_ANDROID)
    #include <spdlog/sinks/android_sink.h>
#else
    #include <spdlog/sinks/stdout_color_sinks.h>
#endif

namespace robv::log
{
    namespace
    {
        std::shared_ptr<spdlog::logger> logger;

        spdlog::level::level_enum to_spdlog(level_e level)
        {
            switch (level)
            {
            case level_e::trace: return spdlog::level::trace;
            case level_e::debug: return spdlog::level::debug;
            case level_e::info: return spdlog::level::info;
            case level_e::warn: return spdlog::level::warn;
            case level_e::error: return spdlog::level::err;
            case level_e::fatal: return spdlog::level::critical;
            }
            return spdlog::level::info;
        }
    } // namespace

    void init(const char* tag)
    {
        if (logger) { return; }

#if defined(ROBV_PLATFORM_ANDROID)
        logger = spdlog::android_logger_mt("robv", tag);
        logger->set_pattern("%v");
#else
        logger = spdlog::stdout_color_mt(tag);
        logger->set_pattern("%H:%M:%S.%e %^%-5l%$ %v");
#endif
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::warn);
    }

    void shutdown()
    {
        if (!logger) { return; }

        logger->flush();
        spdlog::drop(logger->name());
        logger.reset();
    }

    void set_level(level_e level)
    {
        if (logger) { logger->set_level(to_spdlog(level)); }
    }

    bool parse_level(std::string_view text, level_e& out)
    {
        struct level_name_t
        {
            std::string_view name;
            level_e level;
        };

        constexpr level_name_t names[] = {
            {"trace", level_e::trace},
            {"debug", level_e::debug},
            {"info",  level_e::info },
            {"warn",  level_e::warn },
            {"error", level_e::error},
            {"fatal", level_e::fatal},
        };

        for (const level_name_t& entry : names)
        {
            if (entry.name == text)
            {
                out = entry.level;
                return true;
            }
        }
        return false;
    }

    void write(level_e level, const char* category, std::string_view msg)
    {
        // before init (tests) spdlog's default logger takes it
        if (!logger)
        {
            spdlog::log(to_spdlog(level), "[{}] {}", category, msg);
            return;
        }
        logger->log(to_spdlog(level), "[{}] {}", category, msg);
    }
} // namespace robv::log
