#include "json.hpp"
#include "video_core.hpp"
#include "url_utils.hpp"
#include "video_links.hpp"
#include "playlist.hpp"
#include "queue_priority.hpp"

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
    if (argc == 3 && std::string(argv[1]) == "--expand-links") {
        try {
            const auto urls = cryget::expand_video_links(argv[2]);
            for (const auto& url : urls) std::cout << url << '\n';
            return urls.empty() ? 1 : 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    const std::string playlist = "https://www.youtube.com/watch?v=m0FPp52mCwc&list=PLuidrAcAGAOPK_Zp79ljIlP9lYD2PVaq_";
    for (const auto& url : {playlist, playlist + "#t=10", std::string("https://youtu.be/m0FPp52mCwc?list=PLuidrAcAGAOPK_Zp79ljIlP9lYD2PVaq_")}) {
        if (cryget::youtube_playlist_id(url) != "PLuidrAcAGAOPK_Zp79ljIlP9lYD2PVaq_" ||
            !cryget::contains_youtube_playlist(url)) return 10;
    }
    if (cryget::youtube_playlist_url("https://notyoutube.com/playlist?list=PLuidrAcAGAOPK_Zp79ljIlP9lYD2PVaq_") ||
        cryget::youtube_playlist_url("https://youtube.com/playlist?list=bad%20playlist")) return 11;
    const std::string playlist_page = R"(<script>if(window.ytInitialData) { helper({}); }
      var ytInitialData = {};
      window["ytInitialData"] = {"contents":{"playlistVideoListRenderer":{"contents":[
        {"playlistVideoRenderer":{"videoId":"m0FPp52mCwc","title":{"simpleText":"braces } {"}}},
        {"lockupViewModel":{"contentId":"aaaaaaaaaaa","contentType":"LOCKUP_CONTENT_TYPE_VIDEO"}},
        {"continuationItemRenderer":{"continuationEndpoint":{"continuationCommand":{"token":"next-page"}}}}
      ]}},"sidebar":{"videoRenderer":{"videoId":"bbbbbbbbbbb"},"continuationItemRenderer":{"continuationEndpoint":{"continuationCommand":{"token":"wrong-page"}}}}};</script>)";
    std::vector<std::string> ids;
    std::string continuation;
    cryget::collect_playlist_items(cryget::parse_playlist_page(playlist_page), ids, continuation);
    if (ids != std::vector<std::string>{"m0FPp52mCwc", "aaaaaaaaaaa"} || continuation != "next-page") return 12;
    const std::string next_page = R"({"onResponseReceivedActions":[{"appendContinuationItemsAction":{"continuationItems":[{"playlistVideoRenderer":{"videoId":"ccccccccccc"}}]}}]})";
    continuation.clear();
    cryget::collect_playlist_items(cryget::JsonParser(next_page).parse(), ids, continuation);
    if (ids.size() != 3 || ids.back() != "ccccccccccc" || !continuation.empty()) return 13;
    const std::string changed_wrapper = R"({"contents":[{"newYouTubeWrapper":[{"playlistVideoRenderer":{"videoId":"ddddddddddd"}}]}]})";
    cryget::collect_playlist_items(cryget::JsonParser(changed_wrapper).parse(), ids, continuation);
    if (ids.size() != 4 || ids.back() != "ddddddddddd") return 16;
    const auto unknown_renderer = cryget::JsonParser(R"({"newRenderer":{"videoId":"fffffffffff"}})").parse();
    std::vector<std::string> fallback_ids;
    cryget::collect_playlist_video_ids(unknown_renderer, fallback_ids);
    if (fallback_ids != std::vector<std::string>{"fffffffffff"} ||
        cryget::playlist_response_shape(unknown_renderer).find("newRenderer") == std::string::npos) return 18;
    const std::string array_contents = R"(<script>var ytInitialData={"contents":[{"playlistVideoRenderer":{"videoId":"eeeeeeeeeee"}}]};</script>)";
    const auto array_data = cryget::parse_playlist_page(array_contents);
    cryget::collect_playlist_items(array_data, ids, continuation);
    if (ids.size() != 5 || ids.back() != "eeeeeeeeeee") return 17;
    bool missing_data = false;
    try { (void)cryget::parse_playlist_page("<script>if(window.ytInitialData) {}</script>"); }
    catch (const std::runtime_error&) { missing_data = true; }
    if (!missing_data) return 14;
    const std::string mix_page = R"(<script>var ytInitialData={"contents":{"twoColumnWatchNextResults":{"playlist":{"playlist":{"playlistId":"RDXysLGnlSjE0","contents":[{"playlistPanelVideoRenderer":{"videoId":"XysLGnlSjE0","navigationEndpoint":{"watchEndpoint":{"videoId":"XysLGnlSjE0","index":1,"params":"mix-page"}}}},{"playlistPanelVideoRenderer":{"videoId":"aaaaaaaaaaa","navigationEndpoint":{"watchEndpoint":{"videoId":"aaaaaaaaaaa","index":2}}}}]}}}}};</script>)";
    const auto mix = cryget::collect_mix_playlist_page(cryget::parse_playlist_page(mix_page), "RDXysLGnlSjE0");
    if (mix.ids != std::vector<std::string>{"XysLGnlSjE0", "aaaaaaaaaaa"} ||
        mix.last_id != "aaaaaaaaaaa" || mix.next_index != 2 ||
        !cryget::collect_mix_playlist_page(cryget::parse_playlist_page(mix_page), "RDwronglist00").ids.empty()) return 19;
    const auto next_mix = cryget::JsonParser(R"({"contents":{"twoColumnWatchNextResults":{"playlist":{"playlist":{"playlistId":"RDXysLGnlSjE0","contents":[{"playlistPanelVideoRenderer":{"videoId":"aaaaaaaaaaa"}},{"playlistPanelVideoRenderer":{"videoId":"bbbbbbbbbbb","navigationEndpoint":{"watchEndpoint":{"index":3,"params":"next-page"}}}}]}}}}})").parse();
    const auto continued = cryget::collect_mix_playlist_page(next_mix, "RDXysLGnlSjE0");
    if (continued.ids != std::vector<std::string>{"aaaaaaaaaaa", "bbbbbbbbbbb"} ||
        continued.last_id != "bbbbbbbbbbb" || continued.next_index != 3 ||
        continued.next_params != "next-page") return 20;
    if (cryget::youtube_playlist_id("https://www.youtube.com/watch?v=XysLGnlSjE0&list=RDXysLGnlSjE0") !=
        "RDXysLGnlSjE0") return 21;
    const std::vector<int> queue{9, 1, 2, 3};
    if (cryget::priority_jobs(queue, 2, [](int item) { return item != 9; }) != std::vector<int>{1, 2} ||
        !cryget::priority_jobs(queue, 0, [](int) { return true; }).empty()) return 15;
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
