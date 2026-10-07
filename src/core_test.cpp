#include "json.hpp"
#include "video_core.hpp"
#include "url_utils.hpp"
#include "video_links.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--inspect-id") {
        const auto video = cryget::inspect_video(argv[2]);
        const auto format = cryget::choose_format(video, 0, false);
        if (!format.has_audio || (format.url.empty() && format.cipher.empty())) return 9;
        std::cout << "Video: " << video.title << " | standalone MP4: " << format.height << "p\n";
        return 0;
    }
    const auto parsed = cryget::JsonParser(R"({"text":"中\u6587","escaped":"a\\b","array":[1,true,null]})").parse();
    if (parsed.get("text").string != "中文" || parsed.get("escaped").string != "a\\b" ||
        parsed.get("array").array.size() != 3) return 1;

    const std::string page = R"(<script>var ytInitialPlayerResponse = {"videoDetails":{"title":"Example \u4e2d\u6587"},"streamingData":{"formats":[{"url":"https:\/\/example.test\/360.mp4","mimeType":"video/mp4; codecs=\"avc1, mp4a\"","height":360,"bitrate":1000,"audioQuality":"AUDIO_QUALITY_MEDIUM"},{"url":"https://example.test/720.mp4","mimeType":"video/mp4","height":720,"bitrate":3000,"audioQuality":"AUDIO_QUALITY_MEDIUM"}]}};</script>)";
    const auto video = cryget::parse_watch_page(page, "aaaaaaaaaaa");
    if (video.title != "Example 中文" || cryget::choose_format(video, 360).height != 360 ||
        cryget::choose_format(video, 1080).height != 720) return 2;

    const auto links = cryget::parse_links("https://youtu.be/dQw4w9WgXcQ https://www.youtube.com/watch?v=aaaaaaaaaaa&t=30 https://youtu.be/dQw4w9WgXcQ");
    if (links.size() != 2 || links[0] != "https://www.youtube.com/watch?v=dQw4w9WgXcQ" ||
        links[1] != "https://www.youtube.com/watch?v=aaaaaaaaaaa") return 6;
    if (!cryget::parse_links("https://notyoutube.com/watch?v=dQw4w9WgXcQ").empty()) return 7;
    if (!cryget::parse_links("https://youtu.be/dQw4w9WgXcQ unexpected").empty()) return 8;

    if (!std::filesystem::is_directory(cryget::checked_folder("."))) return 3;
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto unicode_folder = std::filesystem::temp_directory_path() /
                                std::filesystem::u8path("crYGet 路径测试 " + suffix);
    std::filesystem::create_directories(unicode_folder);
    const auto resolved = cryget::checked_folder(cryget::path_utf8(unicode_folder));
    std::filesystem::remove(unicode_folder);
    if (!resolved.is_absolute() || resolved.filename() != unicode_folder.filename()) return 5;
    bool rejected = false;
    try { (void) cryget::checked_folder("folder-that-does-not-exist-cryget"); }
    catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) return 4;
}
