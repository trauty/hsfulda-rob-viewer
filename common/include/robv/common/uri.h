#pragma once

#include "robv/common/types.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace robv
{
    // scheme://authority/path?key=value&key=value
    struct uri_t
    {
        std::string scheme;
        std::string authority;
        std::string path;
        std::vector<std::pair<std::string, std::string>> query;
    };
} // namespace robv

namespace robv::uri
{
    bool parse(std::string_view text, uri_t& out);

    std::string_view query_value(const uri_t& uri, std::string_view key, std::string_view fallback = {});
    // fallback if missing or not a whole number
    u64 query_u64(const uri_t& uri, std::string_view key, u64 fallback);
    f64 query_f64(const uri_t& uri, std::string_view key, f64 fallback);

    bool split_host_port(std::string_view authority, std::string& host, u16& port);
} // namespace robv::uri
