#pragma once

#include "robv/common/types.h"

#include <format>
#include <string_view>

namespace robv::log
{
    enum class level_e : u8
    {
        trace,
        debug,
        info,
        warn,
        error,
        fatal
    };

    // stdout on Linux, logcat under tag on Android
    void init(const char* tag);
    void shutdown();

    void set_level(level_e level);
    bool parse_level(std::string_view text, level_e& out);

    void write(level_e level, const char* category, std::string_view msg);
} // namespace robv::log

#define ROBV_LOG_TRACE(cat, fmtstr, ...)                                                                               \
    robv::log::write(robv::log::level_e::trace, cat, std::format(fmtstr __VA_OPT__(, ) __VA_ARGS__))
#define ROBV_LOG_DEBUG(cat, fmtstr, ...)                                                                               \
    robv::log::write(robv::log::level_e::debug, cat, std::format(fmtstr __VA_OPT__(, ) __VA_ARGS__))
#define ROBV_LOG_INFO(cat, fmtstr, ...)                                                                                \
    robv::log::write(robv::log::level_e::info, cat, std::format(fmtstr __VA_OPT__(, ) __VA_ARGS__))
#define ROBV_LOG_WARN(cat, fmtstr, ...)                                                                                \
    robv::log::write(robv::log::level_e::warn, cat, std::format(fmtstr __VA_OPT__(, ) __VA_ARGS__))
#define ROBV_LOG_ERROR(cat, fmtstr, ...)                                                                               \
    robv::log::write(robv::log::level_e::error, cat, std::format(fmtstr __VA_OPT__(, ) __VA_ARGS__))
#define ROBV_LOG_FATAL(cat, fmtstr, ...)                                                                               \
    robv::log::write(robv::log::level_e::fatal, cat, std::format(fmtstr __VA_OPT__(, ) __VA_ARGS__))
