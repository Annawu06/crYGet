#pragma once

#include "json.hpp"
#include <algorithm>
#include <regex>
#include <string>
#include <vector>

namespace cryget {
inline bool valid_playlist_video_id(const std::string& id) {
    return id.size() == 11 && id.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
}

inline Json parse_playlist_page(const std::string& page) {
    // Pages can mention ytInitialData in helper scripts before assigning it.
    // Locate an assignment, then balance JSON braces without counting strings.
    static const std::regex assignment(R"((?:\bytInitialData|window\s*\[\s*["']ytInitialData["']\s*\])\s*=\s*(\{))");
    for (auto it = std::sregex_iterator(page.begin(), page.end(), assignment);
         it != std::sregex_iterator(); ++it) {
        const auto start = static_cast<std::size_t>(it->position(1));
        int depth = 0;
        bool quoted = false, escaped = false;
        for (auto end = start; end < page.size(); ++end) {
            const char c = page[end];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) {
                try {
                    const auto data = page.substr(start, end - start + 1);
                    auto result = JsonParser(data).parse();
                    const auto& contents = result.get("contents");
                    if ((contents.kind == Json::Kind::Object && !contents.object.empty()) ||
                        (contents.kind == Json::Kind::Array && !contents.array.empty()) ||
                        !result.get("onResponseReceivedActions").array.empty() ||
                        !result.get("alerts").array.empty())
                        return result;
                } catch (const std::runtime_error&) {}
                break;
            }
        }
    }
    throw std::runtime_error("Cannot read YouTube playlist data. The page may require sign-in or consent.");
}

// Browse responses sometimes place playlist items under new renderer wrappers.
// The browseId request is already scoped to one playlist, so collecting videoId
// fields anywhere in that response is a safe final fallback.
inline void collect_playlist_video_ids(const Json& value, std::vector<std::string>& ids) {
    if (value.kind == Json::Kind::Array) {
        for (const auto& entry : value.array) collect_playlist_video_ids(entry, ids);
        return;
    }
    if (value.kind != Json::Kind::Object) return;
    const auto& id = value.get("videoId").string;
    if (valid_playlist_video_id(id)) ids.push_back(id);
    const auto& lockup = value.get("lockupViewModel");
    const auto& content_id = lockup.get("contentId").string;
    if (lockup.get("contentType").string == "LOCKUP_CONTENT_TYPE_VIDEO" &&
        valid_playlist_video_id(content_id)) ids.push_back(content_id);
    for (const auto& [_, entry] : value.object) collect_playlist_video_ids(entry, ids);
}

inline std::string playlist_response_shape(const Json& value) {
    std::vector<std::string> keys;
    const auto scan = [&](const auto& self, const Json& node) -> void {
        if (node.kind == Json::Kind::Array) {
            for (const auto& entry : node.array) self(self, entry);
        } else if (node.kind == Json::Kind::Object) {
            for (const auto& [key, entry] : node.object) {
                if ((key.find("Renderer") != std::string::npos || key == "videoId" ||
                     key == "contents" || key == "continuationContents" || key == "alerts" || key == "error") &&
                    keys.size() < 20 && std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
                self(self, entry);
            }
        }
    };
    scan(scan, value);
    std::string result;
    for (const auto& key : keys) {
        if (!result.empty()) result += ",";
        result += key;
    }
    return result;
}

inline void collect_playlist_items(const Json& value, std::vector<std::string>& ids,
                                   std::string& continuation, bool in_list = false) {
    if (value.kind == Json::Kind::Array) {
        for (const auto& entry : value.array) collect_playlist_items(entry, ids, continuation, in_list);
        return;
    }
    if (value.kind != Json::Kind::Object) return;
    // YouTube moves playlist renderers between wrapper types. Identify the
    // renderer itself instead of requiring a particular parent path.
    for (const auto* name : {"playlistVideoRenderer", "playlistPanelVideoRenderer"}) {
        const auto& id = value.get(name).get("videoId").string;
        if (id.size() == 11 && id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos)
            ids.push_back(id);
    }
    if (in_list) {
        for (const auto* name : {"videoRenderer", "compactVideoRenderer"}) {
            const auto& id = value.get(name).get("videoId").string;
            if (id.size() == 11 && id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos)
                ids.push_back(id);
        }
        const auto& lockup = value.get("lockupViewModel");
        const auto& id = lockup.get("contentId").string;
        if (lockup.get("contentType").string == "LOCKUP_CONTENT_TYPE_VIDEO" && id.size() == 11 &&
            id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos)
            ids.push_back(id);
    }
    const auto& token = value.get("continuationItemRenderer").get("continuationEndpoint")
        .get("continuationCommand").get("token").string;
    if (in_list && continuation.empty()) continuation = token;
    for (const auto& [key, entry] : value.object) {
        const bool list = in_list || key == "playlistVideoListRenderer" || key == "playlistPanelRenderer" ||
            key == "playlistVideoListContinuation" || key == "appendContinuationItemsAction" ||
            key == "playlistRenderer" || key == "playlistVideoList";
        collect_playlist_items(entry, ids, continuation, list);
    }
}

struct MixPlaylistPage {
    std::vector<std::string> ids;
    std::string last_id;
    int next_index = 0;
    std::string next_params = "OAE%3D";
};

inline MixPlaylistPage collect_mix_playlist_page(const Json& data, const std::string& expected_id) {
    const auto& panel = data.get("contents").get("twoColumnWatchNextResults")
        .get("playlist").get("playlist");
    const auto& actual_id = panel.get("playlistId").string;
    if (panel.kind != Json::Kind::Object ||
        (!actual_id.empty() && actual_id != expected_id)) return {};
    MixPlaylistPage result;
    for (const auto& item : panel.get("contents").array) {
        const auto& video = item.get("playlistPanelVideoRenderer");
        const auto& id = video.get("videoId").string;
        if (!valid_playlist_video_id(id)) continue;
        result.ids.push_back(id);
        result.last_id = id;
        const auto& endpoint = video.get("navigationEndpoint").get("watchEndpoint");
        const auto& index = endpoint.get("index");
        if (index.kind == Json::Kind::Number && index.number >= 0 && index.number <= 5000)
            result.next_index = static_cast<int>(index.number);
        else result.next_index = static_cast<int>(result.ids.size());
        const auto& params = endpoint.get("params").string;
        result.next_params = params.empty() ? "OAE%3D" : params;
    }
    return result;
}
} // namespace cryget
