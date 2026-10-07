#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/Xft/Xft.h>
#include <curl/curl.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <clocale>
#include <cstring>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <sys/types.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>

#include "i18n.hpp"
#include "layout.hpp"
#include "diagnostics.hpp"
#include "video_core.hpp"
#include "video_links.hpp"

using cryget::parse_links;

namespace fs = std::filesystem;
extern char** environ;

enum class State { Queued, Connecting, Downloading, Canceling, Complete, Failed, Canceled };

struct Thumbnail {
    int width = 0, height = 0;
    std::vector<unsigned char> rgb;
};

struct Job {
    std::string url, title, quality, folder, saved_path, progress, error;
    State state = State::Queued;
    double percent = 0;
    bool cancel = false;
    std::atomic<bool> stop{false};
    std::shared_ptr<Thumbnail> thumbnail;
    bool preview_done = false;
    size_t saved_size = 0;
};

static size_t previous_codepoint(const std::string& value, size_t cursor) {
    if (!cursor) return 0;
    --cursor;
    while (cursor && (static_cast<unsigned char>(value[cursor]) & 0xc0) == 0x80) --cursor;
    return cursor;
}

static size_t next_codepoint(const std::string& value, size_t cursor) {
    if (cursor >= value.size()) return value.size();
    ++cursor;
    while (cursor < value.size() && (static_cast<unsigned char>(value[cursor]) & 0xc0) == 0x80) ++cursor;
    return cursor;
}

struct Editor {
    std::string value;
    size_t cursor = 0, anchor = 0;

    bool selected() const { return cursor != anchor; }
    std::string selection() const {
        return selected() ? value.substr(std::min(cursor, anchor), std::max(cursor, anchor) - std::min(cursor, anchor)) : "";
    }
    void erase_selection() {
        if (!selected()) return;
        const auto first = std::min(cursor, anchor);
        value.erase(first, std::max(cursor, anchor) - first);
        cursor = anchor = first;
    }
    void insert(const std::string& text) {
        erase_selection();
        value.insert(cursor, text);
        cursor += text.size();
        anchor = cursor;
    }
    void backspace() {
        if (selected()) { erase_selection(); return; }
        const auto before = previous_codepoint(value, cursor);
        value.erase(before, cursor - before);
        cursor = anchor = before;
    }
    void delete_forward() {
        if (selected()) { erase_selection(); return; }
        const auto after = next_codepoint(value, cursor);
        value.erase(cursor, after - cursor);
        anchor = cursor;
    }
    void move(size_t position, bool selecting) {
        cursor = std::min(position, value.size());
        if (!selecting) anchor = cursor;
    }
};

static std::mutex jobs_mutex;
static std::vector<std::shared_ptr<Job>> jobs;
static std::vector<std::shared_ptr<Job>> stopping_jobs;
struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};
static std::vector<Worker> workers;
static std::vector<std::thread> preview_workers;
static std::mutex preview_mutex;
static std::condition_variable preview_ready;
static std::deque<std::shared_ptr<Job>> preview_requests;
static std::atomic<bool> running{true};
static constexpr int concurrency = 2;

static std::string size_text(size_t amount) {
    double value = static_cast<double>(amount);
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    size_t index = 0;
    while (value >= 1024 && index < 4) { value /= 1024; ++index; }
    std::ostringstream output;
    output << std::fixed << std::setprecision(index ? 1 : 0) << value << ' ' << units[index];
    return output.str();
}

static void reorder_job(std::shared_ptr<Job> source, std::shared_ptr<Job> target, bool after) {
    if (!source || !target || source == target) return;
    std::lock_guard lock(jobs_mutex);
    auto from = std::find(jobs.begin(), jobs.end(), source);
    auto to = std::find(jobs.begin(), jobs.end(), target);
    if (from == jobs.end() || to == jobs.end()) return;
    jobs.erase(from);
    to = std::find(jobs.begin(), jobs.end(), target);
    jobs.insert(to + (after ? 1 : 0), source);
}

static bool cancel_and_remove_job(const std::shared_ptr<Job>& job) {
    std::lock_guard lock(jobs_mutex);
    const auto entry = std::find(jobs.begin(), jobs.end(), job);
    if (entry == jobs.end()) return false;
    job->cancel = true;
    if (job->state == State::Connecting || job->state == State::Downloading || job->state == State::Canceling) {
        job->state = State::Canceling;
        stopping_jobs.push_back(job);
    } else job->state = State::Canceled;
    job->stop = true;
    jobs.erase(entry);
    cryget::log_event("queue.remove", job->url);
    return true;
}

static void open_folder_in_file_manager(const fs::path& folder) {
    char program[] = "xdg-open";
    const auto path_text = folder.u8string();
    std::vector<char> path(path_text.begin(), path_text.end()); path.push_back('\0');
    char* argv[]{program, path.data(), nullptr};
    pid_t pid = -1;
    const int result = posix_spawnp(&pid, program, nullptr, nullptr, argv, environ);
    if (result == 0) std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
    else cryget::log_event("folder.open.error", path_text + " errno=" + std::to_string(result));
}

static size_t receive_thumbnail(char* data, size_t size, size_t count, void* user) {
    auto* bytes = static_cast<std::vector<unsigned char>*>(user);
    const size_t length = size * count;
    if (length > 2'000'000 || bytes->size() + length > 2'000'000) return 0;
    bytes->insert(bytes->end(), data, data + length);
    return length;
}

static int stop_thumbnail(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return running ? 0 : 1;
}

struct JpegError {
    jpeg_error_mgr base;
    jmp_buf jump;
};

static void jpeg_error_exit(j_common_ptr decoder) {
    longjmp(reinterpret_cast<JpegError*>(decoder->err)->jump, 1);
}

static std::shared_ptr<Thumbnail> decode_jpeg(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return {};
    jpeg_decompress_struct decoder{};
    JpegError error{};
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    auto result = std::make_shared<Thumbnail>();
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&decoder);
        return {};
    }
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, bytes.data(), bytes.size());
    jpeg_read_header(&decoder, TRUE);
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    if (decoder.output_width == 0 || decoder.output_height == 0 || decoder.output_width > 2048 || decoder.output_height > 2048) {
        jpeg_destroy_decompress(&decoder);
        return {};
    }
    result->width = static_cast<int>(decoder.output_width);
    result->height = static_cast<int>(decoder.output_height);
    result->rgb.resize(static_cast<size_t>(result->width) * result->height * 3);
    while (decoder.output_scanline < decoder.output_height) {
        auto* row = result->rgb.data() + static_cast<size_t>(decoder.output_scanline) * result->width * 3;
        jpeg_read_scanlines(&decoder, &row, 1);
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    return result;
}

static std::shared_ptr<Thumbnail> fetch_thumbnail(const std::string& video_url) {
    const auto id = video_url.substr(video_url.find_last_of('=') + 1);
    const std::string url = "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg";
    auto* request = curl_easy_init();
    if (!request) return {};
    std::vector<unsigned char> bytes;
    curl_easy_setopt(request, CURLOPT_URL, url.c_str());
    curl_easy_setopt(request, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(request, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(request, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, receive_thumbnail);
    curl_easy_setopt(request, CURLOPT_WRITEDATA, &bytes);
    curl_easy_setopt(request, CURLOPT_USERAGENT, "crYGet/1.0");
    curl_easy_setopt(request, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(request, CURLOPT_XFERINFOFUNCTION, stop_thumbnail);
    const auto status = curl_easy_perform(request);
    long response = 0;
    curl_easy_getinfo(request, CURLINFO_RESPONSE_CODE, &response);
    curl_easy_cleanup(request);
    return status == CURLE_OK && response == 200 ? decode_jpeg(bytes) : nullptr;
}

static void preview_loop() {
    while (running) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(preview_mutex);
            preview_ready.wait(lock, [] { return !running || !preview_requests.empty(); });
            if (!running) return;
            job = preview_requests.front();
            preview_requests.pop_front();
        }
        {
            std::lock_guard lock(jobs_mutex);
            if (job->cancel) continue;
        }
        auto image = fetch_thumbnail(job->url);
        {
            std::lock_guard lock(jobs_mutex);
            job->thumbnail = image;
        }
        bool canceled = false;
        {
            std::lock_guard lock(jobs_mutex);
            canceled = job->cancel;
        }
        std::shared_ptr<cryget::Video> video;
        if (running && !canceled) {
            try {
                const auto id = job->url.substr(job->url.find_last_of('=') + 1);
                video = std::make_shared<cryget::Video>(cryget::inspect_video(id, &job->stop));
            } catch (const std::exception&) {}
        }
        {
            std::lock_guard lock(jobs_mutex);
            if (video) job->title = video->title;
            job->preview_done = true;
        }
    }
}

static void download(const std::shared_ptr<Job>& job) {
    try {
        cryget::log_event("queue.start", job->url);
        {
            std::lock_guard lock(jobs_mutex);
            if (job->cancel) { job->state = State::Canceled; return; }
        }
        // Media URLs expire; inspect again when a queued download actually starts.
        const auto id = job->url.substr(job->url.find_last_of('=') + 1);
        auto video = std::make_shared<cryget::Video>(cryget::inspect_video(id, &job->stop));
        const int maximum_height = job->quality == "1080p" ? 1080 : job->quality == "720p" ? 720 : 0;
        const auto format = cryget::choose_format(*video, maximum_height);
        const auto folder = cryget::checked_folder(job->folder);
        {
            std::lock_guard lock(jobs_mutex);
            if (job->cancel) { job->state = State::Canceled; return; }
            job->title = video->title;
            job->state = State::Downloading;
        }
        const auto output = cryget::download_video(*video, format, folder, job->stop,
            [job](uint64_t received, uint64_t total) {
                std::lock_guard lock(jobs_mutex);
                if (job->cancel) return;
                job->progress = size_text(received) + " / " + (total ? size_text(total) : "—");
                if (total) job->percent = std::min(100.0, 100.0 * double(received) / double(total));
            });
        std::lock_guard lock(jobs_mutex);
        job->saved_path = output.u8string();
        job->saved_size = static_cast<size_t>(std::filesystem::file_size(output));
        job->progress = size_text(job->saved_size);
        job->percent = 100;
        job->state = State::Complete;
    } catch (const std::exception& error) {
        cryget::log_event("queue.error", job->url + " " + error.what());
        std::lock_guard lock(jobs_mutex);
        job->state = job->cancel ? State::Canceled : State::Failed;
        if (!job->cancel) job->error = error.what();
    }
}

static std::vector<std::shared_ptr<Job>> reserve_downloads() {
    std::vector<std::shared_ptr<Job>> start;
    {
        std::lock_guard lock(jobs_mutex);
        stopping_jobs.erase(std::remove_if(stopping_jobs.begin(), stopping_jobs.end(), [](const auto& job) {
            return job->state != State::Canceling;
        }), stopping_jobs.end());
        int active = static_cast<int>(stopping_jobs.size());
        for (const auto& job : jobs) if (job->state == State::Connecting || job->state == State::Downloading || job->state == State::Canceling) ++active;
        for (const auto& job : jobs) {
            if (active >= concurrency) break;
            if (job->state == State::Queued) { job->state = State::Connecting; start.push_back(job); ++active; }
        }
    }
    return start;
}

static void schedule() {
    for (auto iterator = workers.begin(); iterator != workers.end();) {
        if (iterator->finished->load()) {
            iterator->thread.join();
            iterator = workers.erase(iterator);
        } else ++iterator;
    }
    for (auto& job : reserve_downloads()) {
        auto finished = std::make_shared<std::atomic<bool>>(false);
        workers.push_back({std::thread([job, finished] {
            try { download(job); }
            catch (const std::exception& error) {
                std::lock_guard lock(jobs_mutex);
                job->error = error.what(); job->state = State::Failed;
            }
            finished->store(true);
        }), finished});
    }
}

class App {
    Display* display = nullptr;
    Window window = 0;
    Window link_input = 0;
    GC gc = 0;
    Pixmap frame = 0;
    int frame_width = 0, frame_height = 0;
    XftDraw* draw = nullptr;
    XftFont* font = nullptr;
    XftFont* display_font = nullptr;
    XftFont* heading_font = nullptr;
    mutable std::unordered_map<XftFont*, std::unordered_map<FcChar32, XftFont*>> fallback_fonts;
    
    std::unordered_map<unsigned long, XftColor> text_colors;
    int screen_number = 0;
    Visual* visual = nullptr;
    Colormap colormap = 0;
    Atom clipboard = 0, utf8 = 0, targets_atom = 0, wm_delete = 0;
    XIM input_method = nullptr;
    XIC input_context = nullptr;
    unsigned long page = 0, white = 0, ink = 0, muted = 0, line = 0, blue = 0, pale = 0, green = 0, red = 0;
    int width = 1440, height = 960, scroll = 0;
    bool scroll_drawing = false;
    int pointer_x = -1, pointer_y = -1;
    bool pointer_down = false;
    std::chrono::steady_clock::time_point caret_blink_start = std::chrono::steady_clock::now();
    bool hovering_button = false, hovering_card = false;
    Cursor hand_cursor = 0, text_cursor = 0, move_cursor = 0, current_cursor = 0;
    std::unordered_map<unsigned long long, double> hover_levels;
    int screen_y(int y) const { return y - (scroll_drawing ? scroll : 0); }
    bool caret_visible() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - caret_blink_start).count() % 1000 < 540;
    }
    unsigned long mint = 0, peach = 0, lavender = 0, shadow = 0, hover_fill = 0;
    std::array<unsigned long, 6> brand_colors{};
    Editor links, folder;
    std::string notice_key, owned_clipboard;
    int added_count = 0;
    size_t locale_index = 0;
    bool language_open = false;
    int language_scroll = 0;
    int language_row_height() const { return 88; }
    int language_rows() const { return std::max(1, std::min(static_cast<int>(languages.size()), (height - 176) / language_row_height())); }
    int language_menu_width() const { return std::min(content_width(), 880); }
    bool folder_dialog = false;
    fs::path browse_path;
    std::vector<fs::path> subfolders;
    int folder_scroll = 0;
    std::string quality = "Best";
    enum class Focus { Links, Folder } focus = Focus::Links;
    Focus paste_focus = Focus::Links;
    std::shared_ptr<Job> drag_source, drag_target;
    int drag_y = 0, drag_x = 0, drag_start_x = 0, drag_current_y = 0;
    bool dragging = false;
    std::unordered_map<Job*, Pixmap> image_cache;
    std::array<size_t, 3> visible_link_starts{{0, 0, 0}}, visible_link_ends{{0, 0, 0}};
    size_t visible_folder_start = 0;

    Editor& active_editor() { return focus == Focus::Links ? links : folder; }
    void activate_editor(Focus target) {
        focus = target;
        caret_blink_start = std::chrono::steady_clock::now();
        const auto f = form_layout();
        const auto r = target == Focus::Links ? f.links : f.folder;
        if (r.y < scroll) scroll = std::max(0, r.y - 24);
        else if (r.y + r.h > scroll + height) scroll = r.y + r.h - height + 24;
        position_link_input();
        if (input_context) {
            if (target == Focus::Links) XUnsetICFocus(input_context);
            else XSetICFocus(input_context);
        }
        XSetInputFocus(display, target == Focus::Links ? link_input : window, RevertToParent, CurrentTime);
        XFlush(display);
    }
    void position_link_input() {
        if (!link_input) return;
        const auto r = form_layout().links;
        XMoveResizeWindow(display, link_input, r.x, r.y - scroll, r.w, r.h);
        if (folder_dialog || language_open) XUnmapWindow(display, link_input);
        else XMapWindow(display, link_input);
    }
    void paste_clipboard() {
        auto& editor = active_editor();
        if (XGetSelectionOwner(display, clipboard) == window) { editor.insert(owned_clipboard); return; }
        paste_focus = focus;
        XConvertSelection(display, clipboard, utf8, clipboard, window, CurrentTime);
        XFlush(display);
    }
    void copy_selection(bool cut) {
        auto& editor = active_editor();
        if (!editor.selected()) return;
        owned_clipboard = editor.selection();
        XSetSelectionOwner(display, clipboard, window, CurrentTime);
        if (cut) editor.erase_selection();
    }
    void clipboard_request(const XSelectionRequestEvent& request) {
        XSelectionEvent reply{};
        reply.type = SelectionNotify;
        reply.display = request.display;
        reply.requestor = request.requestor;
        reply.selection = request.selection;
        reply.target = request.target;
        reply.time = request.time;
        reply.property = None;
        const Atom property = request.property == None ? request.target : request.property;
        if (request.target == utf8 || request.target == XA_STRING) {
            XChangeProperty(display, request.requestor, property, request.target, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(owned_clipboard.data()), owned_clipboard.size());
            reply.property = property;
        } else if (request.target == targets_atom) {
            Atom supported[]{utf8, XA_STRING, targets_atom};
            XChangeProperty(display, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(supported), 3);
            reply.property = property;
        }
        XEvent event{};
        event.xselection = reply;
        XSendEvent(display, request.requestor, False, 0, &event);
        XFlush(display);
    }

    std::string locale() const { return languages[locale_index].code; }
    std::string t(const std::string& key) const { return tr(locale(), key); }
    void clear_fallback_fonts() {
        for (auto& [_, characters] : fallback_fonts)
            for (auto& [__, fallback] : characters)
                if (fallback) XftFontClose(display, fallback);
        fallback_fonts.clear();
    }
    XftFont* font_for(FcChar32 character) const {
        if (XftCharExists(display, font, character)) return font;
        auto& characters = fallback_fonts[font];
        const auto found = characters.find(character);
        if (found != characters.end()) return found->second ? found->second : font;

        FcPattern* request = FcPatternCreate();
        FcCharSet* charset = FcCharSetCreate();
        FcCharSetAddChar(charset, character);
        FcPatternAddString(request, FC_FAMILY, reinterpret_cast<const FcChar8*>("sans"));
        double pixels = 40;
        FcPatternGetDouble(font->pattern, FC_PIXEL_SIZE, 0, &pixels);
        FcPatternAddDouble(request, FC_PIXEL_SIZE, pixels);
        FcPatternAddCharSet(request, FC_CHARSET, charset);
        FcCharSetDestroy(charset);
        FcConfigSubstitute(nullptr, request, FcMatchPattern);
        FcDefaultSubstitute(request);
        FcResult result;
        FcPattern* matched = XftFontMatch(display, screen_number, request, &result);
        FcPatternDestroy(request);
        XftFont* fallback = matched ? XftFontOpenPattern(display, matched) : nullptr;
        if (fallback && !XftCharExists(display, fallback, character)) {
            XftFontClose(display, fallback);
            fallback = nullptr;
        }
        characters.emplace(character, fallback);
        return fallback ? fallback : font;
    }
    template <typename Callback>
    void for_text_runs(const std::string& value, Callback callback) const {
        size_t start = 0;
        XftFont* run_font = nullptr;
        for (size_t position = 0; position < value.size();) {
            FcChar32 character = 0;
            const int bytes = FcUtf8ToUcs4(reinterpret_cast<const FcChar8*>(value.data() + position),
                                           &character, static_cast<int>(value.size() - position));
            const size_t next = position + (bytes > 0 ? static_cast<size_t>(bytes) : 1);
            XftFont* current = bytes > 0 ? font_for(character) : font;
            if (run_font && current != run_font) callback(run_font, start, position - start);
            if (current != run_font) { start = position; run_font = current; }
            position = next;
        }
        if (run_font) callback(run_font, start, value.size() - start);
    }
    void select_font() {
        clear_fallback_fonts();
        if (font) XftFontClose(display, font);
        const auto code = locale();
        const char* family = code == "zh-CN" ? "Noto Sans CJK SC:pixelsize=40" :
            code == "zh-HK" ? "Noto Sans CJK HK:pixelsize=40" :
            code == "zh-TW" ? "Noto Sans CJK TC:pixelsize=40" : "Noto Sans:pixelsize=40";
        font = XftFontOpenName(display, screen_number, family);
        if (!font) font = XftFontOpenName(display, screen_number, "sans:pixelsize=40");
    }
    std::string fit_text(const std::string& value, int pixels) const {
        if (!font || pixels < 10) return {};
        if (text_width(value) <= pixels) return value;
        std::string fit;
        for (size_t pos = 0; pos < value.size();) {
            const size_t next = next_codepoint(value, pos);
            const auto candidate = value.substr(0, next) + "…";
            if (text_width(candidate) > pixels) break;
            fit = candidate;
            pos = next;
        }
        return fit.empty() ? "…" : fit;
    }
    int text_width(const std::string& value) const {
        if (!font || value.empty()) return 0;
        int width = 0;
        for_text_runs(value, [&](XftFont* style, size_t start, size_t length) {
            XGlyphInfo extent{};
            XftTextExtentsUtf8(display, style, reinterpret_cast<const FcChar8*>(value.data() + start),
                               static_cast<int>(length), &extent);
            width += extent.xOff;
        });
        return width;
    }
    size_t editor_slice_start(const Editor& editor, size_t start, size_t end, int pixels) const {
        size_t slice = start;
        const size_t cursor = std::clamp(editor.cursor, start, end);
        while (slice < cursor && text_width(editor.value.substr(slice, cursor - slice)) > pixels - 9)
            slice = next_codepoint(editor.value, slice);
        return slice;
    }
    void draw_editor_line(const Editor& editor, size_t start, size_t end, int x, int baseline,
                          int pixels, bool focused, size_t& slice_output) {
        const size_t slice = editor_slice_start(editor, start, end, pixels);
        slice_output = slice;
        const auto visible = fit_text(editor.value.substr(slice, end - slice), pixels);
        if (editor.selected()) {
            const auto left = std::max(slice, std::min(editor.anchor, editor.cursor));
            const auto right = std::min(end, std::max(editor.anchor, editor.cursor));
            if (right > left) {
                const int offset = text_width(editor.value.substr(slice, left - slice));
                const int selected_width = text_width(editor.value.substr(left, right - left));
                box(x + offset, baseline - font->ascent, std::min(selected_width, pixels - offset), font->ascent + font->descent, pale);
            }
        }
        text(x, baseline, visible, ink);
        if (focused && caret_visible() && editor.cursor >= start && editor.cursor <= end) {
            const int offset = text_width(editor.value.substr(slice, editor.cursor - slice));
            box(x + std::min(offset, pixels - 2), baseline - font->ascent, 2, font->ascent + font->descent, blue);
        }
    }
    size_t editor_position_at(const Editor& editor, size_t slice, size_t end, int relative_x) const {
        size_t position = slice;
        while (position < end) {
            const auto next = next_codepoint(editor.value, position);
            if (text_width(editor.value.substr(slice, next - slice)) > relative_x) break;
            position = next;
        }
        return position;
    }
    static unsigned long channel(unsigned char value, unsigned long mask) {
        if (!mask) return 0;
        unsigned shift = 0;
        while (((mask >> shift) & 1UL) == 0) ++shift;
        const auto maximum = mask >> shift;
        return ((static_cast<unsigned long>(value) * maximum + 127) / 255 << shift) & mask;
    }
    void thumbnail(const std::shared_ptr<Job>& job, int x, int y, int tw, int th) {
        box(x, y, tw, th, pale);
        if (!job->thumbnail) {
            text(x + 8, y + 39, fit_text(job->preview_done ? "" : t("preview"), tw - 16), muted);
            return;
        }
        auto found = image_cache.find(job.get());
        if (found == image_cache.end()) {
            auto* image = XCreateImage(display, visual, DefaultDepth(display, screen_number), ZPixmap,
                                       0, nullptr, tw, th, 32, 0);
            if (!image) return;
            image->data = static_cast<char*>(calloc(image->bytes_per_line * th, 1));
            if (!image->data) { XDestroyImage(image); return; }
            const auto& source = *job->thumbnail;
            for (int row = 0; row < th; ++row) {
                for (int column = 0; column < tw; ++column) {
                    const int sx = column * source.width / tw;
                    const int sy = row * source.height / th;
                    const auto offset = (static_cast<size_t>(sy) * source.width + sx) * 3;
                    const auto pixel = channel(source.rgb[offset], visual->red_mask)
                        | channel(source.rgb[offset + 1], visual->green_mask)
                        | channel(source.rgb[offset + 2], visual->blue_mask);
                    XPutPixel(image, column, row, pixel);
                }
            }
            Pixmap pixmap = XCreatePixmap(display, window, tw, th, DefaultDepth(display, screen_number));
            // The page GC clips to the scrolled grid; thumbnail uploads start at (0, 0).
            GC image_gc = XCreateGC(display, pixmap, 0, nullptr);
            XPutImage(display, pixmap, image_gc, image, 0, 0, 0, 0, tw, th);
            XFreeGC(display, image_gc);
            XDestroyImage(image);
            found = image_cache.emplace(job.get(), pixmap).first;
        }
        XCopyArea(display, found->second, frame, gc, 0, 0, tw, th, x, screen_y(y));
    }

    unsigned long color(const char* name) {
        XColor exact{}, screen{};
        XAllocNamedColor(display, colormap, name, &screen, &exact);
        return screen.pixel;
    }
    void box(int x, int y, int w, int h, unsigned long fill, unsigned long border = 0) {
        if (w <= 0 || h <= 0) return;
        y = screen_y(y);
        XSetForeground(display, gc, fill);
        XFillRectangle(display, frame, gc, x, y, w, h);
        if (border) { XSetForeground(display, gc, border); XDrawRectangle(display, frame, gc, x, y, w - 1, h - 1); }
    }
    void text(int x, int y, const std::string& value, unsigned long pixel = 0) {
        if (!font) return;
        auto found = text_colors.find(pixel ? pixel : ink);
        if (found == text_colors.end()) {
            XColor value{}; value.pixel = pixel ? pixel : ink;
            XQueryColor(display, colormap, &value);
            XftColor cached{};
            cached.pixel = value.pixel;
            cached.color = {value.red, value.green, value.blue, 0xffff};
            found = text_colors.emplace(value.pixel, cached).first;
        }
        for_text_runs(value, [&](XftFont* style, size_t start, size_t length) {
            const auto* bytes = reinterpret_cast<const FcChar8*>(value.data() + start);
            XftDrawStringUtf8(draw, &found->second, style, x, screen_y(y), bytes, static_cast<int>(length));
            XGlyphInfo extent{};
            XftTextExtentsUtf8(display, style, bytes, static_cast<int>(length), &extent);
            x += extent.xOff;
        });
    }
    void large_text(int x, int y, const std::string& value, XftFont* style, unsigned long pixel) {
        auto* previous = font;
        if (style) font = style;
        text(x, y, fit_text(value, width - left() - x), pixel);
        font = previous;
    }
    void rounded(int x, int y, int w, int h, unsigned long fill, int radius = 16) {
        if (w <= 0 || h <= 0) return;
        const int r = std::min({radius, w / 2, h / 2});
        box(x + r, y, w - 2*r, h, fill);
        box(x, y + r, w, h - 2*r, fill);
        XSetForeground(display, gc, fill);
        for (int dx : {0, w - 2*r}) for (int dy : {0, h - 2*r})
            XFillArc(display, frame, gc, x + dx, screen_y(y + dy), 2*r, 2*r, 0, 360*64);
    }
    void surface(int x, int y, int w, int h, unsigned long fill, unsigned long border, int radius = 14, int stroke = 1) {
        rounded(x, y, w, h, border, radius);
        rounded(x + stroke, y + stroke, w - 2*stroke, h - 2*stroke, fill, radius - stroke);
    }
    int label_baseline(int y, int h) const {
        return y + (h - (font ? font->ascent + font->descent : 16)) / 2 + (font ? font->ascent : 12);
    }
    unsigned long mix_color(unsigned long a, unsigned long b, double amount) const {
        unsigned long result = 0;
        for (unsigned long mask : {visual->red_mask, visual->green_mask, visual->blue_mask}) {
            if (!mask) continue;
            unsigned shift = 0;
            while (((mask >> shift) & 1UL) == 0) ++shift;
            const auto av = (a & mask) >> shift, bv = (b & mask) >> shift;
            result |= (static_cast<unsigned long>(std::lround(av + (static_cast<double>(bv) - av) * amount)) << shift) & mask;
        }
        return result;
    }
    void button(int x, int y, int w, int h, const std::string& label,
                unsigned long fill, unsigned long foreground, bool enabled = true, bool dropdown = false) {
        const bool hover = enabled && !dragging && !(scroll_drawing && (folder_dialog || language_open)) &&
                           inside(pointer_x, pointer_y, x, screen_y(y), w, h);
        const bool pressed = hover && pointer_down;
        hovering_button = hovering_button || hover;
        const auto id = (static_cast<unsigned long long>(static_cast<unsigned>(x)) << 32) | static_cast<unsigned>(y);
        auto& level = hover_levels[id];
        level += std::clamp((hover ? 1.0 : 0.0) - level, -0.25, 0.25);
        const unsigned long target = foreground == white ? blue : hover_fill;
        const auto background = mix_color(fill, target, pressed ? 1.0 : level);
        const int radius = 16;
        if (level > 0.01) rounded(x - 3, y - 3, w + 6, h + 9, mix_color(page, pale, level), radius + 3);
        surface(x, y, w, h, background, mix_color(line, blue, level), radius, hover ? 2 : 1);
        const int offset = pressed ? 3 : hover ? -1 : 0;
        const int reserve = dropdown ? 28 : 0;
        const auto fitted = fit_text(label, w - 32 - reserve);
        const int tx = dropdown ? x + 16 : x + (w - text_width(fitted)) / 2;
        text(tx, label_baseline(y, h) + offset, fitted, enabled ? foreground : muted);
        if (dropdown) {
            const int ax = x + w - 26, ay = screen_y(y + h / 2 + offset);
            XPoint points[]{{static_cast<short>(ax - 5), static_cast<short>(ay - 2)},
                            {static_cast<short>(ax + 5), static_cast<short>(ay - 2)},
                            {static_cast<short>(ax), static_cast<short>(ay + 3)}};
            XSetForeground(display, gc, foreground);
            XFillPolygon(display, frame, gc, points, 3, Convex, CoordModeOrigin);
        }
    }
    void input_surface(int x, int y, int w, int h, bool focused) {
        if (focused) rounded(x - 3, y - 3, w + 6, h + 6, pale, 17);
        surface(x, y, w, h, white, focused ? blue : line, 14);
    }
    int left() const { return std::clamp(width / 40, 24, 48); }
    int content_width() const { return width - 2 * left(); }
    int columns() const { return content_width() < 2000 ? 1 : 2; }
    int card_gap() const { return std::clamp(width / 28, 36, 56); }
    int card_width() const { return (content_width() - card_gap() * (columns() - 1)) / columns(); }
    int thumb_height() const { return (card_width() - 48) * 9 / 16; }
    int card_height() const { return thumb_height() + 660; }
    int card_x(size_t index) const { return left() + (index % columns()) * (card_width() + card_gap()); }
    int add_width() const { return std::max({192, text_width(t("add")) + 48, text_width(t("paste")) + 48}); }
    int browse_width() const { return std::max(152, text_width(t("browse")) + 40); }
    int action_width() const { return std::max({176, text_width(t("open")) + 40, text_width(t("retry")) + 40, text_width(t("remove")) + 40}); }
    int language_width() const { return std::min(content_width() / 2, std::max(360, text_width(std::string(languages[locale_index].name)) + 72)); }
    void wrapped_text(int x, int y, const std::string& value, int pixels, int rows, unsigned long pixel) {
        size_t start = 0;
        for (int row = 0; row < rows && start < value.size(); ++row) {
            if (row == rows - 1) { text(x, y + row * 72, fit_text(value.substr(start), pixels), pixel); break; }
            size_t end = start, last_space = start;
            while (end < value.size()) {
                const auto next = next_codepoint(value, end);
                if (text_width(value.substr(start, next - start)) > pixels) break;
                if (value[end] == ' ') last_space = end;
                end = next;
            }
            if (end == start) end = next_codepoint(value, start);
            else if (end < value.size() && last_space > start) end = last_space;
            text(x, y + row * 72, value.substr(start, end - start), pixel);
            start = end;
            while (start < value.size() && value[start] == ' ') ++start;
        }
    }
    int section_gap() const { return std::clamp(height / 16, 48, 88); }
    int composer_top() const { return 352 + section_gap() / 2; }
    FormLayout form_layout() const { return FormLayout(left(), composer_top(), content_width(), add_width(), browse_width()); }
    int library_top() const { const auto f = form_layout(); return f.panel.y + f.panel.h + 128; }
    int grid_top() const { return library_top() + 272; }
    int page_height(size_t count) const {
        const int rows = static_cast<int>((count + columns() - 1) / columns());
        return grid_top() + (count ? rows * (card_height() + card_gap()) : 360) + 64;
    }

    std::vector<std::shared_ptr<Job>> snapshot() {
        std::lock_guard lock(jobs_mutex);
        return jobs;
    }
    static bool inside(int x, int y, int bx, int by, int bw, int bh) {
        return x >= bx && x < bx + bw && y >= by && y < by + bh;
    }
    int card_y(size_t index) const { return grid_top() + static_cast<int>(index / columns()) * (card_height() + card_gap()); }
    std::shared_ptr<Job> card_at(int x, int y) {
        if (y < 0 || y >= height) return {};
        y += scroll;
        if (y < grid_top()) return {};
        auto list = snapshot();
        for (size_t i = 0; i < list.size(); ++i)
            if (inside(x, y, card_x(i), card_y(i), card_width(), card_height())) return list[i];
        return {};
    }
    void clamp_scroll() {
        const auto count = snapshot().size();
        scroll = std::clamp(scroll, 0, std::max(0, page_height(count) - height));
    }
    void read_folders() {
        subfolders.clear();
        folder_scroll = 0;
        std::error_code error;
        for (fs::directory_iterator it(browse_path, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            std::error_code status_error;
            if (it->is_directory(status_error) && !status_error) subfolders.push_back(it->path());
        }
        std::sort(subfolders.begin(), subfolders.end(), [](const auto& left, const auto& right) {
            return left.filename().string() < right.filename().string();
        });
    }
    void show_folder_dialog() {
        std::error_code error;
        browse_path = fs::is_directory(folder.value, error) ? fs::path(folder.value) : fs::path(getenv("HOME") ? getenv("HOME") : ".");
        read_folders();
        language_open = false;
        folder_dialog = true;
    }
    Rect folder_panel() const { return {24, 24, width - 48, height - 48}; }
    int folder_rows() const { return std::max(1, (folder_panel().h - 408) / 80); }
    void paint_folder_dialog() {
        const auto p = folder_panel();
        surface(p.x, p.y, p.w, p.h, white, line, 20);
        text(p.x + 24, p.y + 64, fit_text(t("choose_folder"), p.w - 48), ink);
        text(p.x + 24, p.y + 140, fit_text(browse_path.string(), p.w - 48), muted);
        button(p.x + 24, p.y + 168, 280, 80, t("up"), pale, blue);
        const int rows = folder_rows();
        for (int row = 0; row < rows; ++row) {
            const int item = folder_scroll + row;
            if (item >= static_cast<int>(subfolders.size())) break;
            const int ry = p.y + 280 + row * 80;
            button(p.x + 24, ry, p.w - 48, 80, subfolders[item].filename().string(), row % 2 ? white : page, ink);
        }
        const int bw = (p.w - 72) / 2;
        button(p.x + 24, p.y + p.h - 104, bw, 80, t("cancel"), white, blue);
        button(p.x + 48 + bw, p.y + p.h - 104, bw, 80, t("use"), blue, white);
    }
    void click_folder_dialog(int px, int py) {
        const auto p = folder_panel();
        const int bw = (p.w - 72) / 2;
        if (inside(px, py, p.x + 24, p.y + 168, 280, 80)) {
            if (browse_path.has_parent_path() && browse_path != browse_path.root_path()) {
                browse_path = browse_path.parent_path(); read_folders();
            }
        } else if (inside(px, py, p.x + 24, p.y + 280, p.w - 48, folder_rows() * 80)) {
            const int item = folder_scroll + (py - p.y - 280) / 80;
            if (item >= 0 && item < static_cast<int>(subfolders.size())) {
                browse_path = subfolders[item]; read_folders();
            }
        } else if (inside(px, py, p.x + 48 + bw, p.y + p.h - 104, bw, 80)) {
            folder.value = browse_path.string(); folder.cursor = folder.anchor = folder.value.size(); folder_dialog = false;
            activate_editor(Focus::Folder);
        } else if (inside(px, py, p.x + 24, p.y + p.h - 104, bw, 80)) folder_dialog = false;
    }
    void paint() {
        hovering_button = hovering_card = false;
        position_link_input();
        if (width != frame_width || height != frame_height) {
            Pixmap next = XCreatePixmap(display, window, width, height, DefaultDepth(display, screen_number));
            XftDrawChange(draw, next);
            if (frame) XFreePixmap(display, frame);
            frame = next; frame_width = width; frame_height = height;
            for (auto& [_, image] : image_cache) XFreePixmap(display, image);
            image_cache.clear();
        }
        auto list = snapshot();
        int active = 0, waiting = 0, done = 0;
        {
            std::lock_guard lock(jobs_mutex);
            for (const auto& job : list) {
                if (job->state == State::Connecting || job->state == State::Downloading || job->state == State::Canceling) ++active;
                else if (job->state == State::Queued) ++waiting;
                else if (job->state == State::Complete) ++done;
            }
        }
        const int l = left(), w = content_width(), right = l + w;
        const int library = library_top();
        scroll_drawing = false;
        box(0, 0, width, height, page);
        scroll_drawing = true;
        auto* previous = font;
        font = display_font ? display_font : font;
        int bx = l;
        const std::string brand = "crYGet";
        for (size_t i = 0; i < brand.size(); ++i) {
            const auto letter = brand.substr(i, 1);
            text(bx, 90, letter, brand_colors[i]); bx += text_width(letter);
        }
        font = previous;
        button(right - language_width(), 48, language_width(), 96, languages[locale_index].name, white, ink, true, true);
        large_text(l, 232, t("hero"), display_font, ink);
        text(l, 316, fit_text(t("subtitle"), w), muted);

        const auto f = form_layout();
        surface(f.panel.x, f.panel.y, f.panel.w, f.panel.h, white, line, 24);
        text(f.links.x, f.links.y - 28, t("links"), muted);
        input_surface(f.links.x, f.links.y, f.links.w, f.links.h, focus == Focus::Links);
        const int text_x = f.links.x + 20;
        const int first_baseline = f.links.y + 20 + font->ascent;
        const int row_height = 72;
        if (links.value.empty()) {
            text(text_x, first_baseline, fit_text(t("placeholder"), f.links.w - 40), muted);
            visible_link_starts.fill(0); visible_link_ends.fill(0);
            if (focus == Focus::Links && caret_visible()) box(text_x, f.links.y + 20, 2, font->ascent + font->descent, blue);
        } else {
            std::vector<std::pair<size_t, size_t>> lines;
            size_t start = 0;
            while (true) {
                const auto end = links.value.find('\n', start);
                lines.emplace_back(start, end == std::string::npos ? links.value.size() : end);
                if (end == std::string::npos) break;
                start = end + 1;
            }
            size_t current = 0;
            for (size_t i = 0; i < lines.size(); ++i)
                if (links.cursor >= lines[i].first && links.cursor <= lines[i].second) { current = i; break; }
            const size_t first = current > 1 ? current - 2 : 0;
            for (size_t row = 0; row < 3; ++row) {
                const size_t index = first + row;
                if (index >= lines.size()) {
                    visible_link_starts[row] = visible_link_ends[row] = links.value.size(); continue;
                }
                visible_link_ends[row] = lines[index].second;
                draw_editor_line(links, lines[index].first, lines[index].second, text_x,
                                 first_baseline + static_cast<int>(row) * row_height, f.links.w - 40,
                                 focus == Focus::Links, visible_link_starts[row]);
            }
        }
        button(f.paste.x, f.paste.y, f.paste.w, f.paste.h, t("paste"), white, blue);
        button(f.add.x, f.add.y, f.add.w, f.add.h, t("add"), pale, blue);
        text(f.quality.x, f.quality.y - 28, t("quality"), muted);
        button(f.quality.x, f.quality.y, f.quality.w, f.quality.h,
               quality == "Best" ? t("best") : quality, lavender, ink, true, true);
        text(f.folder.x, f.folder.y - 28, t("save"), muted);
        input_surface(f.folder.x, f.folder.y, f.folder.w, f.folder.h, focus == Focus::Folder);
        draw_editor_line(folder, 0, folder.value.size(), f.folder.x + 20,
                         label_baseline(f.folder.y, f.folder.h), f.folder.w - 40,
                         focus == Focus::Folder, visible_folder_start);
        button(f.browse.x, f.browse.y, f.browse.w, f.browse.h, t("browse"), mint, green);
        if (!notice_key.empty()) text(l, f.panel.y + f.panel.h + 70,
            fit_text(t(notice_key) + (notice_key == "added" ? ": " + std::to_string(added_count) : ""), w),
            notice_key == "added" ? green : red);
        large_text(l, library + 64, t("library"), heading_font, ink);
        const int logs_width = std::max(220, text_width(t("logs")) + 40);
        button(right - logs_width, library + 12, logs_width, 96, t("logs"), mint, green);
        wrapped_text(l, library + 168, std::to_string(active) + "/2 " + t("active_count") + "  ·  " +
             std::to_string(waiting) + " " + t("waiting_count") + "  ·  " +
             std::to_string(done) + " " + t("done_count"), w, 2, muted);

        if (list.empty()) {
            rounded(l, grid_top(), w, 360, pale, 28);
            large_text(l + 36, grid_top() + 110, t("empty"), heading_font, ink);
            wrapped_text(l + 36, grid_top() + 210, t("hint"), w - 72, 2, muted);
        }
        {
            std::lock_guard list_lock(jobs_mutex);
            int waiting_position = 0;
            for (size_t i = 0; i < list.size(); ++i) {
                const auto& job = list[i];
                if (job->state == State::Queued) ++waiting_position;
                const int x = card_x(i), y = card_y(i), cw = card_width(), ch = card_height(), th = thumb_height();
                if (y - scroll > height || y + ch < scroll) continue;
                const bool over_card = !folder_dialog && !language_open && inside(pointer_x, pointer_y, x, screen_y(y), cw, ch);
                hovering_card = hovering_card || over_card;
                rounded(x, y + (over_card ? 7 : 3), cw, ch, over_card ? line : shadow, 20);
                surface(x, y, cw, ch, job == drag_source && dragging ? lavender : white,
                        job == drag_target || over_card ? blue : line, 20, job == drag_target ? 2 : 1);
                thumbnail(job, x + 24, y + 24, cw - 48, th);
                const auto id = job->url.substr(job->url.find_last_of('=') + 1);
                wrapped_text(x + 24, y + th + 88, job->title.empty() ? id : job->title, cw - 48, 3, ink);
                std::string state;
                switch (job->state) {
                    case State::Queued: state = t("queued") + " " + std::to_string(waiting_position); break;
                    case State::Connecting: state = t("connecting"); break;
                    case State::Downloading: state = t("downloading") + " " + std::to_string(static_cast<int>(job->percent)) + "%"; break;
                    case State::Canceling: state = t("canceling"); break;
                    case State::Complete: state = t("complete"); break;
                    case State::Failed: state = t("failed"); break;
                    case State::Canceled: state = t("canceled"); break;
                }
                const auto tint = job->state == State::Complete ? mint : job->state == State::Failed ? peach : pale;
                rounded(x + 24, y + th + 280, cw - 48, 88, tint, 8);
                text(x + 36, label_baseline(y + th + 280, 88), fit_text(state, cw - 72), job->state == State::Failed ? red : job->state == State::Complete ? green : blue);
                wrapped_text(x + 24, y + th + 440, job->error.empty() ? job->progress : job->error, cw - 48, 2, muted);
                const bool retry = job->state == State::Failed;
                for (int row = 0; row < 3; ++row) {
                    box(x + 24, y + ch - 84 + row * 10, 3, 3, muted);
                    box(x + 35, y + ch - 84 + row * 10, 3, 3, muted);
                }
                text(x + 46, label_baseline(y + ch - 120, 96), std::to_string(i + 1), muted);
                button(x + cw - 24 - action_width(), y + ch - 120, action_width(), 96,
                       job->state == State::Complete ? t("open") : retry ? t("retry") : t("remove"), white, blue);
                if (job->state == State::Downloading) {
                    box(x + 24, y + th + 26, cw - 48, 3, pale);
                    box(x + 24, y + th + 26, static_cast<int>((cw - 48) * std::clamp(job->percent, 0.0, 100.0) / 100), 3, blue);
                }
            }
        }
        scroll_drawing = false;
        const int total = page_height(list.size());
        if (total > height) {
            const int track = height - 32;
            const int bar = std::max(36, track * height / total);
            rounded(width - 16, 16 + scroll * (track - bar) / (total - height), 6, bar, line, 3);
        }
        if (language_open) {
            const int mw = language_menu_width(), mx = right - mw, rows = language_rows();
            surface(mx, 144, mw, rows * 88 + 16, white, line, 16);
            for (int row = 0; row < rows; ++row) {
                const size_t index = static_cast<size_t>(language_scroll + row);
                if (index >= languages.size()) break;
                const int ry = 152 + row * 88;
                const bool over_row = inside(pointer_x, pointer_y, mx + 8, ry, mw - 16, 88);
                hovering_button = hovering_button || over_row;
                if (index == locale_index || over_row) rounded(mx + 8, ry, mw - 16, 88, over_row ? hover_fill : pale, 12);
                text(mx + 24, label_baseline(ry, 88), fit_text(languages[index].name, mw - 48), index == locale_index ? blue : ink);
            }
        }
        scroll_drawing = false;
        if (folder_dialog) paint_folder_dialog();
        const Cursor cursor = hovering_button ? hand_cursor : (hovering_card || dragging) ? move_cursor : 0;
        if (cursor != current_cursor) {
            if (cursor) XDefineCursor(display, window, cursor);
            else XUndefineCursor(display, window);
            current_cursor = cursor;
        }
        XCopyArea(display, frame, window, gc, 0, 0, width, height, 0, 0);
        XFlush(display);
    }
    void add() {
        auto parsed = parse_links(links.value);
        if (parsed.empty()) { notice_key = "invalid"; return; }
        try { folder.value = cryget::checked_folder(folder.value).u8string(); }
        catch (const std::exception& error) {
            cryget::log_event("queue.folder_error", error.what());
            notice_key = "folder_invalid";
            return;
        }
        int added = 0;
        std::vector<std::shared_ptr<Job>> added_jobs;
        {
            std::lock_guard lock(jobs_mutex);
            for (const auto& url : parsed) {
                const bool exists = std::any_of(jobs.begin(), jobs.end(), [&](const auto& job) {
                    return job->url == url && job->state != State::Failed;
                });
                if (exists) continue;
                auto job = std::make_shared<Job>();
                job->url = url; job->quality = quality; job->folder = folder.value;
                jobs.push_back(job); added_jobs.push_back(job); ++added;
            }
        }
        {
            std::lock_guard lock(preview_mutex);
            for (const auto& job : added_jobs) preview_requests.push_back(job);
        }
        preview_ready.notify_all();
        notice_key = added ? "added" : "duplicate";
        added_count = added;
        if (added) { links.value.clear(); links.cursor = links.anchor = 0; }
        schedule();
    }
    void action(const std::shared_ptr<Job>& job) {
        if (!job) return;
        std::string open;
        bool remove = false;
        {
            std::lock_guard lock(jobs_mutex);
            if (job->state == State::Complete) open = job->folder;
            else if (job->state == State::Failed) {
                job->state = State::Queued; job->cancel = false; job->stop = false;
                job->error.clear(); job->progress.clear(); job->percent = 0;
                job->saved_size = 0;
            } else remove = true;
        }
        if (remove && cancel_and_remove_job(job)) {
            auto image = image_cache.find(job.get());
            if (image != image_cache.end()) { XFreePixmap(display, image->second); image_cache.erase(image); }
            clamp_scroll();
        }
        if (!open.empty()) open_folder_in_file_manager(fs::u8path(open));
        schedule();
    }
    void click(int x, int y) {
        const int right = left() + content_width();
        if (folder_dialog) { click_folder_dialog(x, y); return; }
        const int viewport_y = y;
        y += scroll;
        if (language_open) {
            if (inside(x, viewport_y, right - language_menu_width(), 152, language_menu_width(), language_rows() * 88)) {
                const auto index = static_cast<size_t>(language_scroll + (viewport_y - 152) / 88);
                if (index < languages.size()) { locale_index = index; select_font(); }
                language_open = false; return;
            }
            language_open = false;
        }
        if (inside(x, y, right - language_width(), 48, language_width(), 96)) {
            language_scroll = std::clamp(static_cast<int>(locale_index), 0, std::max(0, static_cast<int>(languages.size()) - language_rows()));
            language_open = true; return;
        }
        const int logs_width = std::max(220, text_width(t("logs")) + 40);
        if (inside(x, y, right - logs_width, library_top() + 12, logs_width, 96)) {
            cryget::log_event("log.open", "user requested log folder");
            open_folder_in_file_manager(cryget::log_path().parent_path());
            return;
        }
        const auto f = form_layout();
        if (f.paste.contains(x, y)) { activate_editor(Focus::Links); paste_clipboard(); return; }
        if (f.add.contains(x, y)) { add(); return; }
        if (f.links.contains(x, y)) {
            activate_editor(Focus::Links);
            const int row = std::clamp((y - f.links.y - 20) / 72, 0, 2);
            links.move(editor_position_at(links, visible_link_starts[row], visible_link_ends[row], x - f.links.x - 20), false);
            return;
        }
        if (f.quality.contains(x, y)) {
            quality = quality == "Best" ? "1080p" : quality == "1080p" ? "720p" : "Best"; return;
        }
        if (f.browse.contains(x, y)) { show_folder_dialog(); return; }
        if (f.folder.contains(x, y)) {
            activate_editor(Focus::Folder);
            folder.move(editor_position_at(folder, visible_folder_start, folder.value.size(), x - f.folder.x - 20), false); return;
        }
        auto job = card_at(x, viewport_y);
        if (!job) return;
        auto list = snapshot();
        const auto index = static_cast<size_t>(std::find(list.begin(), list.end(), job) - list.begin());
        if (inside(x, y, card_x(index) + card_width() - 24 - action_width(), card_y(index) + card_height() - 120, action_width(), 96)) { action(job); return; }
        drag_source = job; drag_target.reset(); drag_y = drag_current_y = viewport_y; drag_x = drag_start_x = x; dragging = false;
    }
    void release(int x, int y) {
        if (dragging && drag_source) {
            auto target = card_at(x, y);
            if (target && target != drag_source) {
                auto list = snapshot();
                const auto index = static_cast<size_t>(std::find(list.begin(), list.end(), target) - list.begin());
                const bool after = columns() == 1 ? y + scroll > card_y(index) + card_height() / 2
                                                   : x > card_x(index) + card_width() / 2;
                reorder_job(drag_source, target, after);
            }
        }
        drag_source.reset(); drag_target.reset(); dragging = false;
    }
    void key(XKeyEvent event) {
        caret_blink_start = std::chrono::steady_clock::now();
        KeySym symbol = 0;
        std::vector<char> buffer(256);
        int count = 0;
        if (input_context && focus != Focus::Links) {
            Status status = XLookupNone;
            count = Xutf8LookupString(input_context, &event, buffer.data(), buffer.size(), &symbol, &status);
            if (status == XBufferOverflow) {
                buffer.resize(static_cast<size_t>(count) + 1);
                count = Xutf8LookupString(input_context, &event, buffer.data(), buffer.size(), &symbol, &status);
            }
            if (status != XLookupChars && status != XLookupBoth) count = 0;
        } else count = XLookupString(&event, buffer.data(), buffer.size(), &symbol, nullptr);
        if (symbol == NoSymbol) symbol = XLookupKeysym(&event, 0);
        if (symbol == XK_Escape) { folder_dialog = false; language_open = false; return; }
        if (folder_dialog) return;
        const bool control = event.state & ControlMask;
        const bool selecting = event.state & ShiftMask;
        auto& editor = active_editor();
        if (control && (symbol == XK_a || symbol == XK_A)) { editor.anchor = 0; editor.cursor = editor.value.size(); return; }
        if (control && (symbol == XK_c || symbol == XK_C)) { copy_selection(false); return; }
        if (control && (symbol == XK_x || symbol == XK_X)) { copy_selection(true); return; }
        if (control && (symbol == XK_v || symbol == XK_V)) { paste_clipboard(); return; }
        if (control && symbol == XK_Return) { add(); return; }
        if (symbol == XK_Tab) { activate_editor(focus == Focus::Links ? Focus::Folder : Focus::Links); return; }
        if (symbol == XK_BackSpace) { editor.backspace(); return; }
        if (symbol == XK_Delete) { editor.delete_forward(); return; }
        if (symbol == XK_Left) { editor.move(previous_codepoint(editor.value, editor.cursor), selecting); return; }
        if (symbol == XK_Right) { editor.move(next_codepoint(editor.value, editor.cursor), selecting); return; }
        if (symbol == XK_Home || symbol == XK_End) {
            size_t position;
            if (control || focus == Focus::Folder) position = symbol == XK_Home ? 0 : editor.value.size();
            else if (symbol == XK_Home) {
                const auto found = editor.cursor ? editor.value.rfind('\n', editor.cursor - 1) : std::string::npos;
                position = found == std::string::npos ? 0 : found + 1;
            } else position = editor.value.find('\n', editor.cursor);
            editor.move(position == std::string::npos ? editor.value.size() : position, selecting);
            return;
        }
        if (focus == Focus::Links && (symbol == XK_Up || symbol == XK_Down)) {
            const auto start_pos = editor.cursor ? editor.value.rfind('\n', editor.cursor - 1) : std::string::npos;
            const size_t start = start_pos == std::string::npos ? 0 : start_pos + 1;
            const size_t column = editor.cursor - start;
            if (symbol == XK_Up && start) {
                const size_t previous_end = start - 1;
                const auto previous_break = previous_end ? editor.value.rfind('\n', previous_end - 1) : std::string::npos;
                const size_t previous_start = previous_break == std::string::npos ? 0 : previous_break + 1;
                editor.move(std::min(previous_start + column, previous_end), selecting);
            } else if (symbol == XK_Down) {
                const auto end = editor.value.find('\n', editor.cursor);
                if (end != std::string::npos) {
                    const size_t next_start = end + 1;
                    const auto next_end = editor.value.find('\n', next_start);
                    editor.move(std::min(next_start + column, next_end == std::string::npos ? editor.value.size() : next_end), selecting);
                }
            }
            return;
        }
        if (symbol == XK_Return) {
            if (focus == Focus::Links) editor.insert("\n");
            else focus = Focus::Links;
            return;
        }
        if (count > 0 && !control && !(event.state & Mod1Mask) && static_cast<unsigned char>(buffer[0]) >= 32) {
            editor.insert(std::string(buffer.data(), static_cast<size_t>(count)));
        }
    }
public:
    App() {
        std::setlocale(LC_CTYPE, "");
        cryget::log_event("app.start", std::string("Linux libcurl=") + curl_version());
        XSetLocaleModifiers("");
        const char* home = getenv("HOME");
        folder.value = home ? std::string(home) + "/Downloads" : ".";
        if (!fs::is_directory(folder.value)) folder.value = home ? home : ".";
        folder.cursor = folder.anchor = folder.value.size();
        display = XOpenDisplay(nullptr);
        if (!display) return;
        screen_number = DefaultScreen(display);
        width = std::min(width, std::max(760, DisplayWidth(display, screen_number) - 80));
        height = std::min(height, std::max(600, DisplayHeight(display, screen_number) - 80));
        visual = DefaultVisual(display, screen_number);
        colormap = DefaultColormap(display, screen_number);
        window = XCreateSimpleWindow(display, RootWindow(display, screen_number), 100, 80, width, height, 0, 0, 0);
        XSizeHints limits{};
        limits.flags = PMinSize;
        limits.min_width = 760;
        limits.min_height = 600;
        XSetWMNormalHints(display, window, &limits);
        XStoreName(display, window, "crYGet");
        long event_mask = ExposureMask | StructureNotifyMask | FocusChangeMask | KeyPressMask | KeyReleaseMask |
                          ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask;
        XSelectInput(display, window, event_mask);
        XSetWindowAttributes input_attributes{};
        input_attributes.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
        link_input = XCreateWindow(display, window, 0, 0, 1, 1, 0, 0, InputOnly,
                                   CopyFromParent, CWEventMask, &input_attributes);
        XWMHints hints{};
        hints.flags = InputHint;
        hints.input = True;
        XSetWMHints(display, window, &hints);
        wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display, window, &wm_delete, 1);
        clipboard = XInternAtom(display, "CLIPBOARD", False);
        utf8 = XInternAtom(display, "UTF8_STRING", False);
        targets_atom = XInternAtom(display, "TARGETS", False);
        input_method = XOpenIM(display, nullptr, nullptr, nullptr);
        if (input_method) {
            input_context = XCreateIC(input_method, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                                      XNClientWindow, window, XNFocusWindow, window, nullptr);
            if (input_context) {
                long filter_events = 0;
                XGetICValues(input_context, XNFilterEvents, &filter_events, nullptr);
                XSelectInput(display, window, event_mask | filter_events);
            }
        }
        hand_cursor = XCreateFontCursor(display, XC_hand2);
        text_cursor = XCreateFontCursor(display, XC_xterm);
        move_cursor = XCreateFontCursor(display, XC_fleur);
        XDefineCursor(display, link_input, text_cursor);
        gc = XCreateGC(display, window, 0, nullptr);
        frame = XCreatePixmap(display, window, width, height, DefaultDepth(display, screen_number));
        frame_width = width;
        frame_height = height;
        draw = XftDrawCreate(display, frame, visual, colormap);
        select_font();
        display_font = XftFontOpenName(display, screen_number, "Noto Sans CJK SC:pixelsize=56:weight=bold");
        heading_font = XftFontOpenName(display, screen_number, "Noto Sans CJK SC:pixelsize=48:weight=bold");
        page = color("#F5F6F8"); white = color("#FFFFFF"); ink = color("#20232B"); muted = color("#646B78");
        line = color("#D9DEE7"); blue = color("#345AA3"); pale = color("#E5EDFC");
        green = color("#28705B"); red = color("#A83F48");
        hover_fill = color("#CDDEFA");
        mint = color("#DFF1E9"); peach = color("#FBE5DF"); lavender = color("#ECE7F6"); shadow = color("#E9ECF1");
        brand_colors = {color("#4285F4"), color("#EA4335"), color("#FBBC05"), color("#4285F4"), color("#34A853"), color("#EA4335")};
        curl_global_init(CURL_GLOBAL_DEFAULT);
        for (int i = 0; i < 4; ++i) preview_workers.emplace_back(preview_loop);
        XMapWindow(display, window);
    }
    ~App() {
        running = false;
        preview_ready.notify_all();
        {
            std::lock_guard lock(jobs_mutex);
            for (auto& job : jobs) {
                if (job->state == State::Connecting || job->state == State::Downloading || job->state == State::Canceling) {
                    job->cancel = true;
                    job->stop = true;
                }
                job->stop = true;
            }
        }
        if (display) {
            for (auto& worker : preview_workers) if (worker.joinable()) worker.join();
            for (auto& worker : workers) if (worker.thread.joinable()) worker.thread.join();
            for (auto& [_, image] : image_cache) XFreePixmap(display, image);
            curl_global_cleanup();
            if (input_context) XDestroyIC(input_context);
            if (input_method) XCloseIM(input_method);
            clear_fallback_fonts();
            if (font) XftFontClose(display, font);
            if (display_font) XftFontClose(display, display_font);
            if (heading_font) XftFontClose(display, heading_font);
            if (draw) XftDrawDestroy(draw);
            if (frame) XFreePixmap(display, frame);
            if (gc) XFreeGC(display, gc);
            if (hand_cursor) XFreeCursor(display, hand_cursor);
            if (text_cursor) XFreeCursor(display, text_cursor);
            if (move_cursor) XFreeCursor(display, move_cursor);
            XDestroyWindow(display, window); XCloseDisplay(display);
        }
    }
    int run() {
        if (!display) { fprintf(stderr, "Cannot open X11 display.\n"); return 1; }
        while (running) {
            while (XPending(display)) {
                XEvent event{};
                XNextEvent(display, &event);
                if (event.xany.window == link_input) {
                    const auto r = form_layout().links;
                    if (event.type == ButtonPress || event.type == ButtonRelease) {
                        event.xbutton.x += r.x; event.xbutton.y += r.y - scroll;
                    } else if (event.type == MotionNotify) {
                        event.xmotion.x += r.x; event.xmotion.y += r.y - scroll;
                    }
                }
                // URL entry uses the local keyboard mapping, independent of XIM filtering.
                if (event.type == KeyPress && focus == Focus::Links && !folder_dialog && !language_open) {
                    key(event.xkey);
                    continue;
                }
                if (input_context && event.type == FocusIn && focus != Focus::Links) XSetICFocus(input_context);
                if (input_context && event.type == FocusOut) XUnsetICFocus(input_context);
                if (XFilterEvent(&event, window)) continue;
                if (event.type == MotionNotify) { pointer_x = event.xmotion.x; pointer_y = event.xmotion.y; }
                else if (event.type == LeaveNotify) { pointer_x = pointer_y = -1; }
                else if (event.type == ButtonPress || event.type == ButtonRelease) {
                    pointer_x = event.xbutton.x; pointer_y = event.xbutton.y;
                    if (event.xbutton.button == Button1) pointer_down = event.type == ButtonPress;
                }
                if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == wm_delete) running = false;
                else if (event.type == ConfigureNotify) { width = event.xconfigure.width; height = event.xconfigure.height; clamp_scroll(); }
                else if (event.type == ButtonPress) {
                    if (event.xbutton.button == Button4) {
                        if (language_open) language_scroll = std::max(0, language_scroll - 1);
                        else if (folder_dialog) folder_scroll = std::max(0, folder_scroll - 1);
                        else { scroll -= 60; clamp_scroll(); }
                    }
                    else if (event.xbutton.button == Button5) {
                        if (language_open) language_scroll = std::min(std::max(0, static_cast<int>(languages.size()) - language_rows()), language_scroll + 1);
                        else if (folder_dialog) folder_scroll = std::min(std::max(0, static_cast<int>(subfolders.size()) - folder_rows()), folder_scroll + 1);
                        else { scroll += 60; clamp_scroll(); }
                    }
                    else if (event.xbutton.button == Button1) click(event.xbutton.x, event.xbutton.y);
                } else if (event.type == MotionNotify && drag_source) {
                    drag_x = event.xmotion.x;
                    drag_current_y = event.xmotion.y;
                    if (std::abs(event.xmotion.y - drag_y) > 5 || std::abs(event.xmotion.x - drag_start_x) > 5) dragging = true;
                    drag_target = dragging ? card_at(event.xmotion.x, event.xmotion.y) : nullptr;
                    if (drag_target == drag_source) drag_target.reset();
                } else if (event.type == ButtonRelease && event.xbutton.button == Button1) release(event.xbutton.x, event.xbutton.y);
                else if (event.type == KeyPress) key(event.xkey);
                else if (event.type == SelectionRequest) clipboard_request(event.xselectionrequest);
                else if (event.type == SelectionNotify && event.xselection.property == None && event.xselection.target == utf8) {
                    XConvertSelection(display, clipboard, XA_STRING, clipboard, window, CurrentTime);
                }
                else if (event.type == SelectionNotify && event.xselection.property != None) {
                    Atom type; int format; unsigned long size, left; unsigned char* data = nullptr;
                    if (XGetWindowProperty(display, window, clipboard, 0, 262144, True, AnyPropertyType,
                                           &type, &format, &size, &left, &data) == Success && data && format == 8) {
                        (paste_focus == Focus::Links ? links : folder).insert(std::string(reinterpret_cast<char*>(data), size));
                    }
                    if (data) XFree(data);
                }
            }
            if (dragging && !folder_dialog) {
                if (drag_current_y > height - 55) scroll += 14;
                else if (drag_current_y < 64) scroll -= 14;
                clamp_scroll();
                drag_target = card_at(drag_x, drag_current_y);
                if (drag_target == drag_source) drag_target.reset();
            }
            schedule();
            paint();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return 0;
    }
};

static int self_test() {
    // Verify the same rectangles used for drawing and clicks across window sizes,
    // long translated labels and scrolled pages.
    for (int window_width : {760, 960, 1280, 1920}) {
        for (int action_size : {192, 280}) {
            const int margin = std::clamp(window_width / 40, 24, 48);
            FormLayout layout(margin, 250, window_width - 2 * margin, action_size, 192);
            const std::array<Rect, 6> controls{{layout.links, layout.paste, layout.add,
                                               layout.quality, layout.folder, layout.browse}};
            for (size_t i = 0; i < controls.size(); ++i) {
                const auto& r = controls[i];
                if (r.w < 128 || r.h < 64 || r.x < layout.panel.x + 32 ||
                    r.x + r.w > layout.panel.x + layout.panel.w - 32 ||
                    r.y + r.h > layout.panel.y + layout.panel.h - 32) return 28;
                for (int page_scroll : {0, 160, 300}) {
                    const int screen_y = r.y + r.h / 2 - page_scroll;
                    if (!r.contains(r.x + r.w / 2, screen_y + page_scroll)) return 29;
                }
                for (size_t j = i + 1; j < controls.size(); ++j) {
                    const auto& other = controls[j];
                    if (r.x < other.x + other.w && r.x + r.w > other.x &&
                        r.y < other.y + other.h && r.y + r.h > other.y) return 30;
                }
            }
            if (layout.links.h < 3 * 72 + 40 || layout.quality.h < 80) return 31;
        }
    }
    Editor editor;
    editor.insert("ab\n中");
    if (editor.value != "ab\n中" || editor.cursor != editor.value.size()) return 14;
    editor.backspace();
    if (editor.value != "ab\n" || editor.cursor != 3) return 15;
    editor.move(0, true);
    if (editor.selection() != "ab\n") return 16;
    editor.insert("new");
    if (editor.value != "new" || editor.cursor != 3 || editor.selected()) return 17;
    auto links = parse_links("https://youtu.be/dQw4w9WgXcQ\nhttps://www.youtube.com/watch?v=aaaaaaaaaaa");
    if (links.size() != 2 || links[0] != "https://www.youtube.com/watch?v=dQw4w9WgXcQ") return 1;
    if (!parse_links("https://notyoutube.com/watch?v=dQw4w9WgXcQ").empty()) return 2;
    std::string oversized;
    for (int index = 0; index < 101; ++index) oversized += "https://youtu.be/dQw4w9WgXcQ\n";
    if (!parse_links(oversized).empty()) return 10;
    auto first = std::make_shared<Job>(), second = std::make_shared<Job>(), third = std::make_shared<Job>();
    first->state = State::Complete;
    jobs = {first, second, third};
    reorder_job(first, third, true);
    if (jobs != std::vector<std::shared_ptr<Job>>{second, third, first}) return 3;
    reorder_job(third, second, false);
    if (jobs != std::vector<std::shared_ptr<Job>>{third, second, first}) return 4;
    const std::string sample_page = R"(<script>var ytInitialPlayerResponse = {"videoDetails":{"title":"Test \u4e2d\u6587"},"streamingData":{"formats":[{"url":"https://example.test/360.mp4","mimeType":"video/mp4","height":360,"bitrate":400000,"audioQuality":"AUDIO_QUALITY_MEDIUM"},{"url":"https://example.test/720.mp4","mimeType":"video/mp4","height":720,"bitrate":900000,"audioQuality":"AUDIO_QUALITY_MEDIUM"}]}};</script>)";
    const auto video = cryget::parse_watch_page(sample_page, "dQw4w9WgXcQ");
    if (video.title != "Test 中文") return 5;
    if (cryget::choose_format(video, 720).height != 720 || cryget::choose_format(video, 360).height != 360) return 6;
    if (cryget::checked_folder("/tmp").empty()) return 11;
    static const std::array<std::string, 36> keys{{"subtitle", "links", "placeholder", "add", "quality", "best",
        "save", "hint", "empty", "queued", "connecting", "downloading", "canceling", "complete", "failed",
        "canceled", "open", "retry", "cancel", "invalid", "folder_invalid", "duplicate", "added", "preview",
        "browse", "choose_folder", "up", "use", "active_count", "waiting_count", "done_count",
        "remove", "hero", "library", "paste", "logs"}};
    for (const auto& language : languages) {
        for (const auto& key : keys) if (messages(language.code).find(key) == messages(language.code).end()) return 7;
    }
    if (decode_jpeg({1, 2, 3})) return 8;
    jpeg_compress_struct encoder{};
    jpeg_error_mgr jpeg_error{};
    encoder.err = jpeg_std_error(&jpeg_error);
    jpeg_create_compress(&encoder);
    unsigned char* encoded = nullptr;
    unsigned long encoded_size = 0;
    jpeg_mem_dest(&encoder, &encoded, &encoded_size);
    encoder.image_width = 2; encoder.image_height = 2; encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_start_compress(&encoder, TRUE);
    unsigned char row[]{255, 0, 0, 0, 255, 0};
    while (encoder.next_scanline < encoder.image_height) {
        unsigned char* scanline = row;
        jpeg_write_scanlines(&encoder, &scanline, 1);
    }
    jpeg_finish_compress(&encoder);
    jpeg_destroy_compress(&encoder);
    std::vector<unsigned char> sample(encoded, encoded + encoded_size);
    free(encoded);
    auto decoded = decode_jpeg(sample);
    if (!decoded || decoded->width != 2 || decoded->height != 2 || decoded->rgb.size() != 12) return 9;
    // Ten tasks: two active, then a drag changes which waiting task fills the next slot.
    jobs.clear();
    std::string batch;
    for (int i = 0; i < 10; ++i) {
        auto job = std::make_shared<Job>();
        job->url = "https://youtu.be/aaaaaaaaaa" + std::to_string(i);
        batch += job->url + "\n";
        jobs.push_back(job);
    }
    if (parse_links(batch).size() != 10) return 22;
    const auto initial = reserve_downloads();
    if (initial.size() != 2 || !reserve_downloads().empty()) return 24;
    auto promoted = jobs.back();
    reorder_job(promoted, jobs[2], false);
    if (!reserve_downloads().empty()) return 25;
    initial.front()->state = State::Complete;
    const auto next = reserve_downloads();
    if (next.size() != 1 || next.front() != promoted || !reserve_downloads().empty()) return 26;
    reorder_job(initial.front(), jobs.back(), true);
    if (initial.front()->state != State::Complete || jobs.back() != initial.front()) return 27;
    auto waiting = *std::find_if(jobs.begin(), jobs.end(), [](const auto& job) {
        return job->state == State::Queued;
    });
    if (!cancel_and_remove_job(waiting) || std::find(jobs.begin(), jobs.end(), waiting) != jobs.end() ||
        waiting->state != State::Canceled) return 32;
    auto active = next.front();
    if (!cancel_and_remove_job(active) || std::find(jobs.begin(), jobs.end(), active) != jobs.end() ||
        stopping_jobs.size() != 1 || active->state != State::Canceling) return 33;
    active->state = State::Canceled;
    reserve_downloads();
    if (!stopping_jobs.empty()) return 34;
    jobs.clear();
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--self-test") return self_test();
    App app;
    return app.run();
}
