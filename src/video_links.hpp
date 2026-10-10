#pragma once

#include <regex>
#include "url_utils.hpp"
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace cryget {

inline std::string youtube_url(const std::string& value) {
    static const std::regex url(R"(^https?://(www\.|m\.|music\.)?(youtube\.com|youtube-nocookie\.com)/(watch\?[^#]*\bv=|shorts/|live/|embed/)([A-Za-z0-9_-]{11})(?:[&#?/].*)?$|^https?://(www\.)?youtu\.be/([A-Za-z0-9_-]{11})(?:[?#/].*)?$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(value, match, url)) return {};
    return "https://www.youtube.com/watch?v=" + (match[4].matched ? match[4].str() : match[6].str());
}

inline std::string youtube_playlist_id(const std::string& value) {
    static const std::regex route(R"(^https?://(?:(?:www\.|m\.|music\.)?youtube\.com/(?:playlist|watch)|(?:www\.)?youtu\.be/[A-Za-z0-9_-]{11})\?[^\s]*$)", std::regex::icase);
    if (!std::regex_match(value, route)) return {};
    const auto id = url_parameter(value, "list");
    if (id.size() < 10 || id.size() > 80) return {};
    for (unsigned char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return {};
    return id;
}

inline bool youtube_playlist_url(const std::string& value) {
    return !youtube_playlist_id(value).empty();
}

inline bool contains_youtube_playlist(const std::string& input) {
    std::istringstream stream(input);std::string token;
    while(stream>>token)if(youtube_playlist_url(token))return true;
    return false;
}

inline std::vector<std::string> parse_links(const std::string& input) {
    std::istringstream stream(input);
    std::string token;
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    size_t count = 0;
    while (stream >> token) {
        auto url = youtube_url(token);
        if (url.empty() || ++count > 100) return {};
        if (seen.insert(url).second) result.push_back(url);
    }
    return result;
}

} // namespace cryget
