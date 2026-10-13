#include "robv/common/uri.h"

#include <charconv>

namespace robv::uri
{
    bool parse(std::string_view text, uri_t& out)
    {
        out = {};

        const size_t colon = text.find("://");
        if (colon == std::string_view::npos || colon == 0) { return false; }

        out.scheme = text.substr(0, colon);
        for (const char c : out.scheme)
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
            if (!ok) { return false; }
        }

        std::string_view rest = text.substr(colon + 3);

        std::string_view query;
        if (const size_t q = rest.find('?'); q != std::string_view::npos)
        {
            query = rest.substr(q + 1);
            rest = rest.substr(0, q);
        }

        const size_t slash = rest.find('/');
        out.authority = rest.substr(0, slash);
        if (slash != std::string_view::npos) { out.path = rest.substr(slash); }

        while (!query.empty())
        {
            const size_t amp = query.find('&');
            const std::string_view pair = query.substr(0, amp);
            query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
            if (pair.empty()) { continue; }

            const size_t eq = pair.find('=');
            if (eq == std::string_view::npos) { out.query.emplace_back(std::string(pair), std::string()); }
            else
            {
                out.query.emplace_back(std::string(pair.substr(0, eq)), std::string(pair.substr(eq + 1)));
            }
        }
        return true;
    }

    std::string_view query_value(const uri_t& uri, std::string_view key, std::string_view fallback)
    {
        for (size_t i = uri.query.size(); i-- > 0;)
        {
            if (uri.query[i].first == key) { return uri.query[i].second; }
        }
        return fallback;
    }

    u64 query_u64(const uri_t& uri, std::string_view key, u64 fallback)
    {
        const std::string_view text = query_value(uri, key);
        u64 value = 0;
        const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return text.empty() || parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ? fallback : value;
    }

    f64 query_f64(const uri_t& uri, std::string_view key, f64 fallback)
    {
        const std::string_view text = query_value(uri, key);
        f64 value = 0.0;
        const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return text.empty() || parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ? fallback : value;
    }

    bool split_host_port(std::string_view authority, std::string& host, u16& port)
    {
        const size_t colon = authority.rfind(':');
        if (colon == std::string_view::npos || colon == 0) { return false; }

        const std::string_view port_text = authority.substr(colon + 1);
        u32 value = 0;
        const std::from_chars_result parsed =
            std::from_chars(port_text.data(), port_text.data() + port_text.size(), value);
        if (port_text.empty() || parsed.ec != std::errc() || parsed.ptr != port_text.data() + port_text.size())
        {
            return false;
        }
        if (value == 0 || value > 65535) { return false; }

        host = authority.substr(0, colon);
        port = static_cast<u16>(value);
        return true;
    }
} // namespace robv::uri
