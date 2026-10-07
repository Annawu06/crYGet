#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cryget {

struct Format {
    std::string url;
    std::string mime_type;
    int height = 0;
    int bitrate = 0;
    bool has_audio = false;
    std::string cipher;
    uint64_t content_length = 0;
    std::string audio_url, audio_cipher;
    uint64_t audio_length = 0;
};

struct Video {
    std::string id;
    std::string title;
    std::vector<Format> formats;
    std::vector<Format> audio_formats;
    std::string player_url;
};

using Progress = std::function<void(uint64_t received, uint64_t total)>;

std::filesystem::path checked_folder(const std::string& utf8);
Video parse_watch_page(const std::string& page, const std::string& id);
Format choose_format(const Video& video, int maximum_height, bool allow_merge = true);
Video inspect_video(const std::string& id, const std::atomic<bool>* canceled = nullptr);
std::filesystem::path download_video(
    const Video& video, const Format& format, const std::filesystem::path& folder,
    std::atomic<bool>& canceled, Progress progress = {});

} // namespace cryget
