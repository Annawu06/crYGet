#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "i18n.hpp"
#include "diagnostics.hpp"
#include "url_utils.hpp"
#include "video_core.hpp"
#include "video_links.hpp"
#include "queue_priority.hpp"
#include "winhttp_download.hpp"

namespace {
constexpr UINT changed_message = WM_APP + 1;
constexpr UINT progress_message = WM_APP + 2;
constexpr UINT playlist_message = WM_APP + 3;
enum Control { Links = 101, Folder, Browse, Quality, Download, List, Remove, Retry, Open, Up, Down, Language, Logs, Paste };
bool ui_self_test = false;

constexpr COLORREF canvas = RGB(245, 247, 251);
constexpr COLORREF ink = RGB(31, 41, 57);
constexpr COLORREF muted = RGB(99, 113, 132);
constexpr COLORREF blue = RGB(52, 89, 181);
constexpr COLORREF line = RGB(219, 225, 234);
constexpr COLORREF white = RGB(255, 255, 255);
constexpr COLORREF lavender = RGB(236, 241, 252);
constexpr COLORREF mint = RGB(226, 242, 235);
constexpr COLORREF peach = RGB(254, 235, 232);
constexpr COLORREF pale_blue = RGB(229, 236, 251);
constexpr COLORREF neutral = RGB(241, 244, 248);

void fill(HDC dc, RECT r, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

void rounded(HDC dc, RECT r, COLORREF color, int radius = 16) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(brush);
}

void rounded_outline(HDC dc, RECT r, COLORREF color, int radius = 16) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

void label(HDC dc, const std::wstring& value, RECT r, COLORREF color, HFONT font, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, value.c_str(), -1, &r, flags);
}

HFONT make_font(int size, int weight = FW_NORMAL) {
    return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) return L"";
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring size_text(uint64_t bytes) {
    double value = static_cast<double>(bytes);
    const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    size_t index = 0;
    while (value >= 1024 && index < 4) { value /= 1024; ++index; }
    std::wostringstream output;
    output << std::fixed << std::setprecision(index ? 1 : 0) << value << L' ' << units[index];
    return output.str();
}

std::wstring time_text(uint64_t seconds) {
    const auto minutes = seconds / 60;
    std::wostringstream output;
    output << minutes << L':' << std::setw(2) << std::setfill(L'0') << seconds % 60;
    return output.str();
}

std::wstring control_text(HWND control) {
    const int count = GetWindowTextLengthW(control);
    std::wstring result(count + 1, L'\0');
    GetWindowTextW(control, result.data(), count + 1);
    result.resize(count);
    return result;
}

// Decode off the UI thread. The returned DIB can be drawn by the window thread.
HBITMAP decode_thumbnail(const std::vector<unsigned char>& bytes, int& width, int& height) {
    if (bytes.empty() || bytes.size() > 2'000'000) return nullptr;
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    HBITMAP bitmap = nullptr;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) break;
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                                static_cast<DWORD>(bytes.size())))) break;
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 2048 || h > 2048) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                         nullptr, 0, WICBitmapPaletteTypeCustom))) break;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(w);
        info.bmiHeader.biHeight = -static_cast<LONG>(h);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap) break;
        if (FAILED(converter->CopyPixels(nullptr, w * 4, w * h * 4, static_cast<BYTE*>(pixels)))) {
            DeleteObject(bitmap);
            bitmap = nullptr;
            break;
        }
        width = static_cast<int>(w);
        height = static_cast<int>(h);
    } while (false);
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return bitmap;
}

HBITMAP fetch_thumbnail(const std::string& video_url, const std::atomic<bool>* canceled, int& width, int& height) {
    const auto id = video_url.substr(video_url.find_last_of('=') + 1);
    std::vector<unsigned char> bytes;
    try {
        const auto response = cryget::winhttp_get("https://i.ytimg.com/vi/" + id + "/mqdefault.jpg", canceled,
            [&](const char* data, size_t count, uint64_t, uint64_t) {
                if (bytes.size() + count > 2'000'000) throw std::runtime_error("Thumbnail too large");
                bytes.insert(bytes.end(), data, data + count);
            });
        if (response.status != 200) return nullptr;
        return decode_thumbnail(bytes, width, height);
    } catch (const std::exception&) {
        return nullptr;
    }
}

struct Task {
    std::string url, title, quality;
    std::filesystem::path folder;
    std::string error, saved_path;
    std::atomic<bool> stop{false};
    bool requeue = false;
    int state = 0; // 0 waiting, 1 connecting, 2 downloading, 3 complete, 4 failed, 5 stopping
    double percent = 0;
    uint64_t received = 0, total = 0, saved_size = 0;
    double speed = 0;
    std::chrono::steady_clock::time_point started{}, speed_sample{}, last_ui_update{};
    uint64_t speed_sample_bytes = 0, elapsed_seconds = 0;
    std::shared_ptr<cryget::Video> video;
    HBITMAP thumbnail = nullptr;
    int thumbnail_width = 0, thumbnail_height = 0;
    bool preview_done = false;
    bool preview_thumbnail_only = false;
    ~Task() { if (thumbnail) DeleteObject(thumbnail); }
};

struct App {
    HWND window = nullptr;
    HWND links = nullptr, folder = nullptr, browse = nullptr, quality = nullptr;
    HWND download = nullptr, list = nullptr, remove = nullptr, retry = nullptr, open = nullptr;
    HWND up = nullptr, down = nullptr, language = nullptr, logs = nullptr, paste = nullptr;
    HWND heading = nullptr, save_label = nullptr, empty = nullptr;
    std::vector<std::shared_ptr<Task>> tasks;
    // The list box stores one row per task; keep its displayed order separately
    // so status updates never have to reset the control or its selection.
    std::vector<std::shared_ptr<Task>> displayed_tasks;
    std::vector<std::shared_ptr<Task>> active;
    std::deque<std::shared_ptr<Task>> preview_queue;
    std::vector<std::thread> preview_workers;
    std::thread playlist_worker;
    std::mutex mutex;
    std::mutex playlist_mutex;
    std::vector<std::string> playlist_urls;
    std::string playlist_error;
    std::filesystem::path playlist_folder;
    std::string playlist_quality;
    bool playlist_ready = false;
    bool playlist_loading = false;
    std::atomic<bool> playlist_cancel{false};
    std::condition_variable stopped;
    std::condition_variable preview_ready;
    std::atomic<bool> closing{false};
    std::atomic<bool> progress_pending{false};
    size_t locale = 0;
    HFONT body_font = nullptr, small_font = nullptr, title_font = nullptr, button_font = nullptr, card_font = nullptr;
    HBRUSH white_brush = nullptr;
    std::shared_ptr<Task> drag_source;
    POINT drag_origin{};
    bool dragging = false;
    int drawn_rows = 0;
    int buffered_paints = 0;
    int list_rebuilds = 0;
    bool painting_list_buffer = false;
    int folder_open_requests = 0;
    int browse_requests = 0;
    std::filesystem::path last_opened_folder;
    bool compact_layout = false;

    std::wstring trw(const char* key) const { return wide(tr(languages[locale].code, key)); }

    void notify() const { if (!closing && window) PostMessageW(window, changed_message, 0, 0); }

    void notify_progress() {
        bool expected = false;
        if (!closing && window && progress_pending.compare_exchange_strong(expected, true))
            if (!PostMessageW(window, progress_message, 0, 0)) progress_pending = false;
    }

    void preview_loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::shared_ptr<Task> task;
            {
                std::unique_lock lock(mutex);
                preview_ready.wait(lock, [&] { return closing || !preview_queue.empty(); });
                if (closing) break;
                task = preview_queue.front();
                preview_queue.pop_front();
            }
            int width = 0, height = 0;
            HBITMAP bitmap = task->stop ? nullptr : fetch_thumbnail(task->url, &task->stop, width, height);
            std::string title;
            if (!task->stop && !closing && !task->preview_thumbnail_only) {
                try {
                    const auto id = task->url.substr(task->url.find_last_of('=') + 1);
                    title = cryget::inspect_video(id, &task->stop).title;
                } catch (const std::exception&) {}
            }
            {
                std::lock_guard lock(mutex);
                if (!closing && !task->stop) {
                    task->thumbnail = bitmap;
                    task->thumbnail_width = width;
                    task->thumbnail_height = height;
                    if (task->title.empty()) task->title = std::move(title);
                    bitmap = nullptr;
                }
                task->preview_done = true;
                notify();
            }
            if (bitmap) DeleteObject(bitmap);
        }
        CoUninitialize();
    }

    void start_previews() {
        for (int i = 0; i < 2; ++i) preview_workers.emplace_back([this] { preview_loop(); });
    }

    // Caller holds mutex. Keep stopping workers counted until they exit.
    void schedule() {
        const auto desired = cryget::priority_jobs(tasks, 2, [](const auto& task) {
            return task->state == 0 || task->state == 1 || task->state == 2 || task->requeue;
        });
        if (ui_self_test) { notify(); return; }
        for (const auto& task : active) {
            if ((task->state == 1 || task->state == 2) &&
                std::find(desired.begin(), desired.end(), task) == desired.end()) {
                task->requeue = true;
                task->state = 5;
                task->stop = true;
            }
        }
        for (const auto& task : desired) {
            if (closing || active.size() >= 2) break;
            if (task->state != 0) continue;
            task->state = 1;
            active.push_back(task);
            std::thread([this, task] { run(task); }).detach();
        }
        notify();
    }

    void run(const std::shared_ptr<Task>& task) {
        try {
            cryget::log_event("queue.start", task->url);
            const auto id = task->url.substr(task->url.find_last_of('=') + 1);
            auto video = std::make_shared<cryget::Video>(cryget::inspect_video(id, &task->stop));
            const int maximum = task->quality == "1080p" ? 1080 : task->quality == "720p" ? 720 : 0;
            auto format = cryget::choose_format(*video, maximum);
            {
                std::lock_guard lock(mutex);
                if (task->stop) throw std::runtime_error("Download interrupted");
                task->title = video->title;
                task->video = video;
                task->state = 2;
                task->started = task->speed_sample = task->last_ui_update = std::chrono::steady_clock::now();
                notify();
            }
            auto output = cryget::download_video(*video, format, task->folder, task->stop,
                [this, task](uint64_t received, uint64_t total) {
                    std::lock_guard lock(mutex);
                    const int old_percent = static_cast<int>(task->percent);
                    const auto now = std::chrono::steady_clock::now();
                    const auto sample_seconds = std::chrono::duration<double>(now - task->speed_sample).count();
                    task->received = received;
                    task->total = total;
                    task->percent = total ? std::min(100.0, 100.0 * double(received) / double(total)) : 0;
                    task->elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - task->started).count();
                    if (sample_seconds >= 0.5) {
                        task->speed = received >= task->speed_sample_bytes ?
                            (received - task->speed_sample_bytes) / sample_seconds : 0;
                        task->speed_sample = now;
                        task->speed_sample_bytes = received;
                    }
                    if (static_cast<int>(task->percent) != old_percent ||
                        now - task->last_ui_update >= std::chrono::milliseconds(250)) {
                        task->last_ui_update = now;
                        notify_progress();
                    }
                });
            {
                std::lock_guard lock(mutex);
                task->saved_path = cryget::path_utf8(output);
                task->saved_size = std::filesystem::file_size(output);
                task->state = 3;
                task->requeue = false;
            }
        } catch (const std::exception& error) {
            cryget::log_event("queue.error", task->url + " " + error.what());
            std::lock_guard lock(mutex);
            if (task->requeue && !closing && std::find(tasks.begin(), tasks.end(), task) != tasks.end()) {
                task->state = 5;
                task->error.clear();
                task->percent = 0;
                task->received = task->total = task->speed_sample_bytes = task->elapsed_seconds = 0;
                task->speed = 0;
            } else {
                task->error = error.what();
                task->state = task->stop ? 5 : 4;
            }
        }
        {
            std::lock_guard lock(mutex);
            active.erase(std::remove(active.begin(), active.end(), task), active.end());
            if (task->requeue && !closing && std::find(tasks.begin(), tasks.end(), task) != tasks.end()) {
                task->requeue = false;
                task->stop = false;
                task->state = 0;
            }
            schedule();
            stopped.notify_all();
        }
    }

    std::shared_ptr<Task> selected() {
        const auto index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        std::lock_guard lock(mutex);
        return index >= 0 && static_cast<size_t>(index) < tasks.size() ? tasks[index] : nullptr;
    }

    void update_actions() {
        const int index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        size_t count = 0;
        int state = -1;
        {
            std::lock_guard lock(mutex);
            count = tasks.size();
            if (index >= 0 && static_cast<size_t>(index) < count) state = tasks[index]->state;
        }
        const bool has_selection = state >= 0;
        EnableWindow(remove, has_selection);
        EnableWindow(retry, state == 4);
        EnableWindow(open, has_selection);
        EnableWindow(up, has_selection && index > 0);
        EnableWindow(down, has_selection && static_cast<size_t>(index + 1) < count);
    }

    void refresh() {
        const int selection = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        const int top_index = static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0));
        const auto selected_task = selection >= 0 && static_cast<size_t>(selection) < displayed_tasks.size()
            ? displayed_tasks[selection] : nullptr;
        std::vector<std::shared_ptr<Task>> current_tasks;
        {
            std::lock_guard lock(mutex);
            current_tasks = tasks;
        }
        if (current_tasks != displayed_tasks) {
            ++list_rebuilds;
            SendMessageW(list, WM_SETREDRAW, FALSE, 0);
            SendMessageW(list, LB_RESETCONTENT, 0, 0);
            displayed_tasks = std::move(current_tasks);
            for (size_t index = 0; index < displayed_tasks.size(); ++index)
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L" "));
            int next_selection = -1;
            if (selected_task) {
                const auto found = std::find(displayed_tasks.begin(), displayed_tasks.end(), selected_task);
                if (found != displayed_tasks.end()) next_selection = static_cast<int>(found - displayed_tasks.begin());
            }
            if (next_selection < 0 && !displayed_tasks.empty())
                next_selection = std::min(selection < 0 ? 0 : selection, static_cast<int>(displayed_tasks.size()) - 1);
            if (next_selection >= 0) SendMessageW(list, LB_SETCURSEL, next_selection, 0);
            if (top_index > 0) SendMessageW(list, LB_SETTOPINDEX, top_index, 0);
            SendMessageW(list, WM_SETREDRAW, TRUE, 0);
            ShowWindow(empty, displayed_tasks.empty() ? SW_SHOW : SW_HIDE);
        }
        InvalidateRect(list, nullptr, FALSE);
        update_actions();
    }

    void invalidate_active_rows() {
        std::vector<int> indexes;
        {
            std::lock_guard lock(mutex);
            for (size_t index = 0; index < tasks.size(); ++index)
                if (tasks[index]->state == 2) indexes.push_back(static_cast<int>(index));
        }
        for (const int index : indexes) {
            RECT row{};
            if (SendMessageW(list, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&row)) != LB_ERR)
                InvalidateRect(list, &row, FALSE);
        }
    }

    void translate() {
        const int selected_quality = static_cast<int>(SendMessageW(quality, CB_GETCURSEL, 0, 0));
        SendMessageW(quality, CB_DELETESTRING, 0, 0);
        const auto best = trw("best");
        SendMessageW(quality, CB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(best.c_str()));
        SendMessageW(quality, CB_SETCURSEL, selected_quality >= 0 ? selected_quality : 0, 0);
        SetWindowTextW(window, L"crYGet");
        SetWindowTextW(heading, trw("links").c_str());
        SetWindowTextW(save_label, trw("save").c_str());
        SetWindowTextW(empty, trw("empty").c_str());
        SetWindowTextW(browse, trw("browse").c_str());
        SetWindowTextW(paste, trw("paste").c_str());
        SetWindowTextW(download, playlist_loading ? trw("playlist_loading").c_str() : trw("add").c_str());
        SetWindowTextW(remove, trw("remove").c_str());
        SetWindowTextW(retry, trw("retry").c_str());
        SetWindowTextW(open, trw("open").c_str());
        SetWindowTextW(logs, trw("logs").c_str());
        SetWindowTextW(up, L"↑");
        SetWindowTextW(down, L"↓");
        refresh();
        RECT client{};
        GetClientRect(window, &client);
        resize(client.right, client.bottom);
    }

    void add() {
        cryget::log_event("queue.add.click", playlist_loading ? "playlist loading" : "received");
        if (playlist_loading) return;
        std::filesystem::path target;
        try { target = cryget::checked_folder(utf8(control_text(folder))); }
        catch (const std::exception& error) {
            cryget::log_event("queue.folder_error", error.what());
            MessageBoxW(window, wide(error.what()).c_str(), L"crYGet", MB_ICONERROR); return;
        }
        const auto input = utf8(control_text(links));
        const bool has_playlist = cryget::contains_youtube_playlist(input);
        const int quality_index = static_cast<int>(SendMessageW(quality, CB_GETCURSEL, 0, 0));
        const std::string quality_value = quality_index == 1 ? "1080p" : quality_index == 2 ? "720p" : "Best";
        if (has_playlist) {
            playlist_loading = true;
            playlist_cancel = false;
            playlist_folder = std::move(target);
            playlist_quality = quality_value;
            SetWindowTextW(download, trw("playlist_loading").c_str());
            EnableWindow(download, FALSE);
            EnableWindow(links, FALSE);
            EnableWindow(paste, FALSE);
            InvalidateRect(window, nullptr, TRUE);
            if (playlist_worker.joinable()) playlist_worker.join();
            const HWND notify_window = window;
            playlist_worker = std::thread([this, input, notify_window] {
                std::vector<std::string> urls;
                std::string error;
                try { urls = cryget::expand_video_links(input, &playlist_cancel); }
                catch (const std::exception& exception) { error = exception.what(); }
                {
                    std::lock_guard lock(playlist_mutex);
                    playlist_urls = std::move(urls);
                    playlist_error = std::move(error);
                    playlist_ready = true;
                }
                if (!closing) PostMessageW(notify_window, playlist_message, 0, 0);
            });
            return;
        }
        std::vector<std::string> urls;
        try { urls = cryget::expand_video_links(input); }
        catch (const std::exception& error) {
            cryget::log_event("playlist.expand_error", error.what());
            MessageBoxW(window, wide(error.what()).c_str(), L"crYGet", MB_ICONERROR);
            return;
        }
        if (urls.empty()) {
            MessageBoxW(window, trw("invalid").c_str(), L"crYGet", MB_ICONERROR);
            return;
        }
        add_urls(urls, std::move(target), quality_value, false);
    }

    void add_urls(const std::vector<std::string>& urls, std::filesystem::path target,
                  const std::string& quality_value, bool has_playlist) {
        if (urls.empty()) {
            MessageBoxW(window, trw("invalid").c_str(), L"crYGet", MB_ICONERROR);
            return;
        }
        size_t added = 0;
        int existing_index = -1;
        int first_added_index = -1;
        {
            std::lock_guard lock(mutex);
            for (const auto& url : urls) {
                // A failed download can be submitted again with a new folder or
                // quality, just as it can on Linux. Keep the failed card for context.
                const auto existing = std::find_if(tasks.begin(), tasks.end(),
                    [&](const auto& task) { return task->url == url && task->state != 4; });
                if (existing != tasks.end()) {
                    if (existing_index < 0) existing_index = static_cast<int>(existing - tasks.begin());
                    continue;
                }
                auto task = std::make_shared<Task>();
                task->url = url; task->folder = target; task->quality = quality_value;
                task->preview_thumbnail_only = has_playlist;
                if (first_added_index < 0) first_added_index = static_cast<int>(tasks.size());
                tasks.push_back(task);
                preview_queue.push_back(task);
                ++added;
            }
            if (added) {
                preview_ready.notify_all();
                schedule();
            }
        }
        cryget::log_event("queue.add.result", "requested=" + std::to_string(urls.size()) +
                          " added=" + std::to_string(added));
        if (!added) {
            refresh();
            if (existing_index >= 0) {
                SendMessageW(list, LB_SETCURSEL, existing_index, 0);
                SendMessageW(list, LB_SETTOPINDEX, existing_index, 0);
                SetFocus(list);
            }
            if (!ui_self_test) MessageBoxW(window, trw("duplicate").c_str(), L"crYGet", MB_ICONINFORMATION);
            return;
        }
        SetWindowTextW(links, L"");
        refresh();
        if (first_added_index >= 0) {
            SendMessageW(list, LB_SETCURSEL, first_added_index, 0);
            SendMessageW(list, LB_SETTOPINDEX, first_added_index, 0);
            update_actions();
        }
        RECT viewport{};
        GetClientRect(list, &viewport);
        cryget::log_event("queue.ui", "items=" + std::to_string(SendMessageW(list, LB_GETCOUNT, 0, 0)) +
                          " viewport_height=" + std::to_string(viewport.bottom) +
                          " row_height=" + std::to_string(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0)));
    }

    void finish_playlist() {
        if (playlist_worker.joinable()) playlist_worker.join();
        std::vector<std::string> urls;
        std::string error;
        {
            std::lock_guard lock(playlist_mutex);
            if (!playlist_ready) return;
            urls = std::move(playlist_urls);
            error = std::move(playlist_error);
            playlist_ready = false;
        }
        playlist_loading = false;
        EnableWindow(download, TRUE);
        EnableWindow(links, TRUE);
        EnableWindow(paste, TRUE);
        SetWindowTextW(download, trw("add").c_str());
        if (!error.empty()) {
            cryget::log_event("playlist.expand_error", error);
            MessageBoxW(window, wide(error).c_str(), L"crYGet", MB_ICONERROR);
        } else add_urls(urls, std::move(playlist_folder), playlist_quality, true);
        InvalidateRect(window, nullptr, TRUE);
    }

    void paste_links() {
        if (!OpenClipboard(window)) return;
        HANDLE contents = GetClipboardData(CF_UNICODETEXT);
        if (contents) {
            const auto* text = static_cast<const wchar_t*>(GlobalLock(contents));
            if (text) {
                SendMessageW(links, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text));
                GlobalUnlock(contents);
                SetFocus(links);
            }
        }
        CloseClipboard();
    }

    void erase() {
        const int selected_index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        auto task = selected();
        if (!task) return;
        size_t remaining = 0;
        {
            std::lock_guard lock(mutex);
            task->stop = true;
            task->requeue = false;
            tasks.erase(std::remove(tasks.begin(), tasks.end(), task), tasks.end());
            remaining = tasks.size();
            cryget::log_event("queue.remove", task->url);
            schedule();
        }
        refresh();
        if (remaining) {
            const int next = std::min(selected_index, static_cast<int>(remaining) - 1);
            SendMessageW(list, LB_SETCURSEL, next, 0);
        }
        update_actions();
    }

    void retry_selected() {
        auto task = selected();
        if (!task) return;
        {
            std::lock_guard lock(mutex);
            if (task->state != 4) return;
            task->state = 0; task->stop = false; task->error.clear(); task->percent = 0;
            task->received = task->total = task->saved_size = task->speed_sample_bytes = task->elapsed_seconds = 0;
            task->speed = 0;
            schedule();
        }
        refresh();
    }

    void move_selected(int direction) {
        auto task = selected();
        if (!task) return;
        std::lock_guard lock(mutex);
        const auto index = std::find(tasks.begin(), tasks.end(), task) - tasks.begin();
        const auto next = index + direction;
        if (next < 0 || next >= static_cast<ptrdiff_t>(tasks.size())) return;
        std::swap(tasks[index], tasks[next]);
        schedule();
        refresh_without_lock(next);
    }

    void refresh_without_lock(ptrdiff_t selection) {
        // Keep UI changes on the window thread after releasing the task lock.
        PostMessageW(window, changed_message, static_cast<WPARAM>(selection + 1), 0);
    }

    void open_folder() {
        auto task = selected();
        if (!task) return;
        launch_folder(task->folder);
    }

    void launch_folder(const std::filesystem::path& target) {
        if (ui_self_test) {
            ++folder_open_requests;
            last_opened_folder = target;
            return;
        }
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", target.c_str(),
                                                      nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
            cryget::log_event("folder.open_error", cryget::path_utf8(target));
            MessageBoxW(window, target.c_str(), L"Could not open folder", MB_OK | MB_ICONERROR);
        }
    }

    void browse_folder() {
        if (ui_self_test) {
            ++browse_requests;
            return;
        }
        IFileOpenDialog* dialog = nullptr;
        HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(result)) {
            DWORD options = 0;
            result = dialog->GetOptions(&options);
            if (SUCCEEDED(result)) result = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            if (SUCCEEDED(result)) result = dialog->SetTitle(trw("choose_folder").c_str());
            const auto current = control_text(folder);
            if (SUCCEEDED(result) && !current.empty()) {
                IShellItem* initial = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(current.c_str(), nullptr, IID_PPV_ARGS(&initial)))) {
                    dialog->SetFolder(initial);
                    initial->Release();
                }
            }
            if (SUCCEEDED(result)) result = dialog->Show(window);
            if (SUCCEEDED(result)) {
                IShellItem* chosen = nullptr;
                result = dialog->GetResult(&chosen);
                if (SUCCEEDED(result)) {
                    PWSTR path = nullptr;
                    result = chosen->GetDisplayName(SIGDN_FILESYSPATH, &path);
                    if (SUCCEEDED(result)) {
                        SetWindowTextW(folder, path);
                        CoTaskMemFree(path);
                    }
                    chosen->Release();
                }
            }
            dialog->Release();
        }
        if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            cryget::log_event("folder.browse_error", "HRESULT=" + std::to_string(result));
            MessageBoxW(window, trw("folder_invalid").c_str(), L"crYGet", MB_OK | MB_ICONERROR);
        }
    }

    void resize(int width, int height) {
        const int margin = 28, gap = 12;
        const int content = width - 2 * margin;
        compact_layout = height < 760;
        const bool narrow = width < 900;
        MoveWindow(language, width - 226, 32, 188, 240, TRUE);
        MoveWindow(heading, 60, compact_layout ? 136 : 150, content - 224, 24, TRUE);
        MoveWindow(paste, width - 204, compact_layout ? 132 : 146, 144, 30, TRUE);
        MoveWindow(links, 60, compact_layout ? 162 : 179, content - 64,
                   compact_layout ? 50 : 76, TRUE);
        MoveWindow(save_label, 260, compact_layout ? 219 : 267, content - 264, 22, TRUE);
        MoveWindow(quality, 60, compact_layout ? 245 : 292, 180, 240, TRUE);
        MoveWindow(folder, 260, compact_layout ? 245 : 292, width - 624,
                   compact_layout ? 34 : 38, TRUE);
        MoveWindow(browse, width - 352, compact_layout ? 245 : 292, 102,
                   compact_layout ? 34 : 38, TRUE);
        MoveWindow(download, width - 232, compact_layout ? 242 : 289, 172,
                   compact_layout ? 40 : 44, TRUE);
        const int list_top = compact_layout ? 360 : 425;
        const int action_top = height - (narrow ? 108 : compact_layout ? 60 : 70);
        MoveWindow(list, 60, list_top, content - 64,
                   std::max(80, action_top - list_top - 16), TRUE);
        RECT list_client{};
        GetClientRect(list, &list_client);
        const int old_row_height = static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0));
        const int top_index = static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0));
        SendMessageW(list, LB_SETITEMHEIGHT, 0,
                     std::min(264, std::max(90, static_cast<int>(list_client.bottom) - 2)));
        if (old_row_height != SendMessageW(list, LB_GETITEMHEIGHT, 0, 0)) {
            SendMessageW(list, LB_SETTOPINDEX, top_index, 0);
            InvalidateRect(list, nullptr, TRUE);
        }
        MoveWindow(empty, 88, compact_layout ? 385 : 450, content - 120,
                   std::max(54, action_top - (compact_layout ? 385 : 450) - 12), TRUE);
        HDC dc = GetDC(window);
        HGDIOBJ old_font = SelectObject(dc, button_font);
        int button_x = 60;
        for (const auto& button : {std::pair<HWND, int>{remove, 140}, {retry, 140},
                                   {open, 140}, {logs, 160}}) {
            const auto caption = control_text(button.first);
            SIZE extent{};
            GetTextExtentPoint32W(dc, caption.c_str(), static_cast<int>(caption.size()), &extent);
            const int button_width = std::max(button.second, static_cast<int>(extent.cx) + 32);
            MoveWindow(button.first, button_x, action_top, button_width, 38, TRUE);
            button_x += button_width + gap;
        }
        SelectObject(dc, old_font);
        ReleaseDC(window, dc);
        MoveWindow(up, width - 150, narrow ? height - 54 : action_top, 38, 38, TRUE);
        MoveWindow(down, width - 100, narrow ? height - 54 : action_top, 38, 38, TRUE);
        InvalidateRect(window, nullptr, TRUE);
    }

    void shutdown() {
        playlist_cancel = true;
        std::unique_lock lock(mutex);
        closing = true;
        for (const auto& task : tasks) task->stop = true;
        for (const auto& task : active) task->stop = true;
        preview_queue.clear();
        preview_ready.notify_all();
        stopped.wait(lock, [&] { return active.empty(); });
        lock.unlock();
        if (playlist_worker.joinable()) playlist_worker.join();
        for (auto& worker : preview_workers) if (worker.joinable()) worker.join();
        window = nullptr;
    }
};

App* app = nullptr;
WNDPROC original_list_proc = nullptr;

LRESULT CALLBACK queue_list_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (!app) return CallWindowProcW(original_list_proc, window, message, wparam, lparam);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC target_dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        HDC buffer_dc = width > 0 && height > 0 ? CreateCompatibleDC(target_dc) : nullptr;
        HBITMAP bitmap = buffer_dc ? CreateCompatibleBitmap(target_dc, width, height) : nullptr;
        HGDIOBJ old_bitmap = bitmap ? SelectObject(buffer_dc, bitmap) : nullptr;
        HDC draw_dc = bitmap ? buffer_dc : target_dc;
        if (bitmap) ++app->buffered_paints;
        fill(draw_dc, client, white);
        const int count = static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0));
        const int top = static_cast<int>(SendMessageW(window, LB_GETTOPINDEX, 0, 0));
        const int selection = static_cast<int>(SendMessageW(window, LB_GETCURSEL, 0, 0));
        app->painting_list_buffer = bitmap != nullptr;
        for (int index = std::max(0, top); index < count; ++index) {
            RECT row{};
            if (SendMessageW(window, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&row)) == LB_ERR) break;
            if (row.top >= client.bottom) break;
            if (row.bottom <= client.top) continue;
            DRAWITEMSTRUCT item{};
            item.CtlType = ODT_LISTBOX;
            item.CtlID = List;
            item.itemID = static_cast<UINT>(index);
            item.itemAction = ODA_DRAWENTIRE;
            if (index == selection) item.itemState |= ODS_SELECTED;
            if (index == selection && GetFocus() == window) item.itemState |= ODS_FOCUS;
            item.hwndItem = window;
            item.hDC = draw_dc;
            item.rcItem = row;
            SendMessageW(app->window, WM_DRAWITEM, List, reinterpret_cast<LPARAM>(&item));
        }
        app->painting_list_buffer = false;
        if (bitmap) {
            BitBlt(target_dc, paint.rcPaint.left, paint.rcPaint.top,
                   paint.rcPaint.right - paint.rcPaint.left,
                   paint.rcPaint.bottom - paint.rcPaint.top,
                   buffer_dc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
            SelectObject(buffer_dc, old_bitmap);
            DeleteObject(bitmap);
        }
        if (buffer_dc) DeleteDC(buffer_dc);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_DELETE) {
        app->erase();
        return 0;
    }
    const int x = static_cast<short>(LOWORD(lparam));
    const int y = static_cast<short>(HIWORD(lparam));
    if (message == WM_LBUTTONDOWN) {
        const auto result = CallWindowProcW(original_list_proc, window, message, wparam, lparam);
        const auto hit = static_cast<DWORD>(SendMessageW(window, LB_ITEMFROMPOINT, 0, lparam));
        if (!HIWORD(hit)) {
            std::lock_guard lock(app->mutex);
            if (LOWORD(hit) < app->tasks.size()) {
                app->drag_source = app->tasks[LOWORD(hit)];
                app->drag_origin = {x, y};
                app->dragging = false;
                SetCapture(window);
            }
        }
        return result;
    }
    if (message == WM_MOUSEMOVE && app->drag_source) {
        if (!app->dragging && (std::abs(x - app->drag_origin.x) >= GetSystemMetrics(SM_CXDRAG) ||
                              std::abs(y - app->drag_origin.y) >= GetSystemMetrics(SM_CYDRAG))) {
            app->dragging = true;
            SetTimer(window, 2, 100, nullptr);
        }
        if (app->dragging) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            const auto hit = static_cast<DWORD>(SendMessageW(window, LB_ITEMFROMPOINT, 0, lparam));
            if (!HIWORD(hit)) SendMessageW(window, LB_SETCURSEL, LOWORD(hit), 0);
        }
        return 0;
    }
    if (message == WM_TIMER && wparam == 2 && app->dragging) {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(window, &cursor);
        RECT client{};
        GetClientRect(window, &client);
        const int direction = cursor.y < 28 ? -1 : cursor.y > client.bottom - 28 ? 1 : 0;
        if (direction) {
            const auto top = SendMessageW(window, LB_GETTOPINDEX, 0, 0);
            SendMessageW(window, LB_SETTOPINDEX, std::max<LRESULT>(0, top + direction), 0);
        }
        return 0;
    }
    if (message == WM_LBUTTONUP && app->drag_source) {
        auto source = app->drag_source;
        const bool dragging = app->dragging;
        app->drag_source.reset();
        app->dragging = false;
        KillTimer(window, 2);
        ReleaseCapture();
        const auto hit = static_cast<DWORD>(SendMessageW(window, LB_ITEMFROMPOINT, 0, lparam));
        if (dragging && !HIWORD(hit)) {
            RECT bounds{};
            SendMessageW(window, LB_GETITEMRECT, LOWORD(hit), reinterpret_cast<LPARAM>(&bounds));
            std::lock_guard lock(app->mutex);
            auto from = std::find(app->tasks.begin(), app->tasks.end(), source);
            if (from != app->tasks.end() && LOWORD(hit) < app->tasks.size()) {
                auto target = app->tasks[LOWORD(hit)];
                if (target != source) {
                    app->tasks.erase(from);
                    auto to = std::find(app->tasks.begin(), app->tasks.end(), target);
                    app->tasks.insert(to + (y > (bounds.top + bounds.bottom) / 2 ? 1 : 0), source);
                    app->schedule();
                }
                app->refresh_without_lock(std::find(app->tasks.begin(), app->tasks.end(), source) - app->tasks.begin());
            }
        }
        return CallWindowProcW(original_list_proc, window, message, wparam, lparam);
    }
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE) {
        app->drag_source.reset();
        app->dragging = false;
        KillTimer(window, 2);
    }
    return CallWindowProcW(original_list_proc, window, message, wparam, lparam);
}

HWND control(HWND parent, const wchar_t* type, const wchar_t* label, DWORD style, int id) {
    HWND result = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style,
                                 0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(app->body_font), TRUE);
    return result;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CREATE) {
        app = new App;
        app->window = window;
        app->body_font = make_font(16);
        app->small_font = make_font(14);
        app->title_font = make_font(27, FW_SEMIBOLD);
        app->button_font = make_font(15, FW_SEMIBOLD);
        app->card_font = make_font(19, FW_SEMIBOLD);
        app->white_brush = CreateSolidBrush(white);
        app->heading = control(window, L"STATIC", L"", 0, 0);
        app->paste = control(window, L"BUTTON", L"", BS_OWNERDRAW, Paste);
        app->links = control(window, L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL, Links);
        SendMessageW(app->links, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
        app->save_label = control(window, L"STATIC", L"", 0, 0);
        app->folder = control(window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, Folder);
        SendMessageW(app->folder, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
        app->browse = control(window, L"BUTTON", L"", BS_OWNERDRAW, Browse);
        app->quality = control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, Quality);
        for (const wchar_t* quality : {L"Best", L"1080p", L"720p"})
            SendMessageW(app->quality, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(quality));
        SendMessageW(app->quality, CB_SETCURSEL, 0, 0);
        app->language = control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, Language);
        for (const auto& language : languages)
            SendMessageW(app->language, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(language.name).c_str()));
        SendMessageW(app->language, CB_SETCURSEL, 0, 0);
        app->download = control(window, L"BUTTON", L"", BS_OWNERDRAW, Download);
        app->list = control(window, L"LISTBOX", L"", WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                            LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, List);
        original_list_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(app->list, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(queue_list_proc)));
        app->empty = control(window, L"STATIC", L"", SS_CENTER | SS_CENTERIMAGE, 0);
        SendMessageW(app->empty, WM_SETFONT, reinterpret_cast<WPARAM>(app->small_font), TRUE);
        app->remove = control(window, L"BUTTON", L"", BS_OWNERDRAW, Remove);
        app->retry = control(window, L"BUTTON", L"", BS_OWNERDRAW, Retry);
        app->open = control(window, L"BUTTON", L"", BS_OWNERDRAW, Open);
        app->logs = control(window, L"BUTTON", L"", BS_OWNERDRAW, Logs);
        app->up = control(window, L"BUTTON", L"", BS_OWNERDRAW, Up);
        app->down = control(window, L"BUTTON", L"", BS_OWNERDRAW, Down);
        wchar_t home[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH)) {
            std::wstring downloads = std::wstring(home) + L"\\Downloads";
            SetWindowTextW(app->folder, std::filesystem::is_directory(downloads) ? downloads.c_str() : home);
        }
        app->translate();
        if (!ui_self_test) app->start_previews();
        SetTimer(window, 1, 1000, nullptr);
        return 0;
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {800, 660};
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(window, &ps);
            RECT client{};
            GetClientRect(window, &client);
            fill(dc, client, canvas);
            RECT header{0, 0, client.right, 116};
            fill(dc, header, white);
            fill(dc, RECT{0, 114, client.right, 116}, line);
            rounded(dc, RECT{28, 25, 101, 98}, lavender, 20);
            DrawIconEx(dc, 32, 25, reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON)),
                       60, 60, 0, nullptr, DI_NORMAL);
            SelectObject(dc, app->title_font);
            SetBkMode(dc, TRANSPARENT);
            const wchar_t* word = L"crYGet";
            const COLORREF brand_colors[] = {
                RGB(74, 123, 202), RGB(201, 111, 118), RGB(214, 168, 79),
                RGB(74, 123, 202), RGB(102, 164, 140), RGB(201, 111, 118)
            };
            int text_x = 110;
            for (int i = 0; word[i]; ++i) {
                SIZE extent{};
                GetTextExtentPoint32W(dc, word + i, 1, &extent);
                SetTextColor(dc, brand_colors[i]);
                TextOutW(dc, text_x, 22, word + i, 1);
                text_x += extent.cx;
            }
            label(dc, app->trw("subtitle"), RECT{111, 58, client.right - 254, 101}, muted, app->small_font,
                  DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
            const bool compact = app->compact_layout;
            const RECT input_panel{28, compact ? 124 : 132, client.right - 28, compact ? 296 : 350};
            rounded(dc, input_panel, white, 20);
            rounded_outline(dc, input_panel, line, 20);
            label(dc, app->trw("quality"), compact ? RECT{60, 218, 240, 241} :
                  RECT{60, 266, 240, 289}, muted, app->small_font);
            const RECT library_panel{28, compact ? 310 : 370, client.right - 28, client.bottom - 16};
            rounded(dc, library_panel, white, 20);
            rounded_outline(dc, library_panel, line, 20);
            label(dc, app->trw("library"), compact ? RECT{60, 318, client.right - 60, 352} :
                  RECT{60, 383, client.right - 60, 417}, ink, app->card_font);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, TRANSPARENT);
            const HWND source = reinterpret_cast<HWND>(lparam);
            SetTextColor(dc, source == app->empty ? muted : ink);
            return reinterpret_cast<LRESULT>(app->white_brush);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkColor(dc, white);
            SetTextColor(dc, ink);
            return reinterpret_cast<LRESULT>(app->white_brush);
        }
        case WM_MEASUREITEM: {
            auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (item->CtlID == List) { item->itemHeight = 264; return TRUE; }
            break;
        }
        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (item->CtlType == ODT_BUTTON) {
                const bool primary = item->CtlID == Download;
                const bool pressed = (item->itemState & ODS_SELECTED) != 0;
                const bool disabled = (item->itemState & ODS_DISABLED) != 0;
                const COLORREF background = disabled ? neutral : primary ? (pressed ? RGB(38, 72, 151) : blue) :
                                            item->CtlID == Remove ? (pressed ? RGB(249, 218, 211) : peach) :
                                            item->CtlID == Browse ? (pressed ? RGB(211, 234, 221) : mint) :
                                            pressed ? lavender : neutral;
                rounded(item->hDC, item->rcItem, background, 12);
                if (!primary) rounded_outline(item->hDC, item->rcItem, line, 12);
                wchar_t name[160]{};
                GetWindowTextW(item->hwndItem, name, 160);
                RECT r = item->rcItem;
                if (pressed) OffsetRect(&r, 0, 1);
                label(item->hDC, name, r, disabled ? muted : primary ? white : ink, app->button_font,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (item->itemState & ODS_FOCUS) {
                    RECT focus = item->rcItem;
                    InflateRect(&focus, -4, -4);
                    DrawFocusRect(item->hDC, &focus);
                }
                return TRUE;
            }
            if (item->CtlID == List && item->itemID != static_cast<UINT>(-1)) {
                ++app->drawn_rows;
                RECT row = item->rcItem;
                RECT viewport{}, visible{};
                GetClientRect(app->list, &viewport);
                if (!IntersectRect(&visible, &row, &viewport)) return TRUE;
                // Paint the complete card offscreen, then present it in one copy.
                // Progress updates otherwise expose the white erase between its shapes.
                HDC target_dc = item->hDC;
                HDC buffer_dc = app->painting_list_buffer ? nullptr : CreateCompatibleDC(target_dc);
                HBITMAP buffer_bitmap = buffer_dc ? CreateCompatibleBitmap(target_dc,
                    row.right - row.left, row.bottom - row.top) : nullptr;
                HGDIOBJ old_bitmap = buffer_bitmap ? SelectObject(buffer_dc, buffer_bitmap) : nullptr;
                DRAWITEMSTRUCT buffered_item = *item;
                if (buffer_bitmap) {
                    SetViewportOrgEx(buffer_dc, -row.left, -row.top, nullptr);
                    buffered_item.hDC = buffer_dc;
                    item = &buffered_item;
                }
                const int saved_clip = SaveDC(item->hDC);
                IntersectClipRect(item->hDC, visible.left, visible.top, visible.right, visible.bottom);
                const bool compact = row.bottom - row.top < 264;
                const bool selected = (item->itemState & ODS_SELECTED) != 0;
                fill(item->hDC, row, white);
                const RECT card{row.left + 8, row.top + 7, row.right - 8, row.bottom - 7};
                rounded(item->hDC, card, selected ? RGB(247, 250, 255) : white, 16);
                rounded_outline(item->hDC, card, selected ? RGB(161, 184, 226) : line, 16);
                std::shared_ptr<Task> task;
                HBITMAP thumbnail = nullptr;
                int thumbnail_width = 0, thumbnail_height = 0, state = 0;
                bool preview_done = false;
                double percent = 0;
                uint64_t received = 0, total = 0, saved_size = 0, elapsed_seconds = 0;
                double speed = 0;
                std::string title, url, error;
                {
                    std::lock_guard lock(app->mutex);
                    if (item->itemID < app->tasks.size()) {
                        task = app->tasks[item->itemID];
                        thumbnail = task->thumbnail;
                        thumbnail_width = task->thumbnail_width;
                        thumbnail_height = task->thumbnail_height;
                        preview_done = task->preview_done;
                        state = task->state;
                        percent = task->percent;
                        received = task->received;
                        total = task->total;
                        saved_size = task->saved_size;
                        elapsed_seconds = task->elapsed_seconds;
                        speed = task->speed;
                        title = task->title;
                        url = task->url;
                        error = task->error;
                    }
                }
                if (task) {
                    const int preview_height = compact ?
                        std::min(180, std::max(1, static_cast<int>(row.bottom - row.top) - 28)) : 180;
                    const int preview_width = compact ? preview_height * 16 / 9 : 320;
                    const RECT preview{row.left + 16, row.top + 14, row.left + 16 + preview_width,
                                       row.top + 14 + preview_height};
                    rounded(item->hDC, preview, pale_blue, 10);
                    rounded_outline(item->hDC, preview, line, 10);
                    if (thumbnail && thumbnail_width > 0 && thumbnail_height > 0) {
                        HDC source = CreateCompatibleDC(item->hDC);
                        if (source) {
                            HGDIOBJ old = SelectObject(source, thumbnail);
                            const int saved = SaveDC(item->hDC);
                            HRGN clip = CreateRoundRectRgn(preview.left, preview.top, preview.right + 1, preview.bottom + 1, 10, 10);
                            ExtSelectClipRgn(item->hDC, clip, RGN_AND);
                            DeleteObject(clip);
                            const int target_width = preview.right - preview.left;
                            const int target_height = preview.bottom - preview.top;
                            int source_width = thumbnail_width;
                            int source_height = thumbnail_height;
                            if (source_width * target_height > source_height * target_width)
                                source_width = source_height * target_width / target_height;
                            else
                                source_height = source_width * target_height / target_width;
                            SetStretchBltMode(item->hDC, HALFTONE);
                            StretchBlt(item->hDC, preview.left, preview.top, target_width, target_height,
                                       source, (thumbnail_width - source_width) / 2,
                                       (thumbnail_height - source_height) / 2, source_width, source_height, SRCCOPY);
                            RestoreDC(item->hDC, saved);
                            SelectObject(source, old);
                            DeleteDC(source);
                        }
                    } else {
                        label(item->hDC, preview_done ? L"▶" : app->trw("preview"), preview, blue,
                              app->small_font, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }
                    const int details_left = compact ? preview.right + 20 : row.left + 358;
                    const COLORREF state_color = state == 3 ? RGB(40, 112, 91) : state == 4 ? RGB(168, 63, 72) : blue;
                    label(item->hDC, wide(title.empty() ? url : title),
                          RECT{details_left, row.top + (compact ? 14 : 30), row.right - 24,
                               compact ? row.bottom - 52 : row.top + 115}, ink, app->card_font,
                          DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
                    const char* key = state == 0 ? "queued" : state == 1 ? "connecting" : state == 2 ? "downloading" :
                                      state == 3 ? "complete" : state == 4 ? "failed" : "canceling";
                    std::wstring status = app->trw(key);
                    if (state == 2) status += total ?
                        L"  " + std::to_wstring(static_cast<int>(percent)) + L"%  ·  " + size_text(received) :
                        L"  ·  " + size_text(received);
                    if (state == 4 && !error.empty()) status += L" · " + wide(error);
                    const RECT status_box{details_left, compact ? row.bottom - 48 : row.top + 133,
                                          row.right - 24, compact ? row.bottom - 12 : row.top + 172};
                    rounded(item->hDC, status_box, state == 3 ? mint : state == 4 ? peach : pale_blue, 8);
                    label(item->hDC, status, RECT{status_box.left + 12, status_box.top + 2,
                          status_box.right - 12, status_box.bottom - 2}, state_color, app->small_font);
                    if (!compact && (state == 2 || state == 3)) {
                        const std::wstring amount = state == 3 ? size_text(saved_size) :
                            size_text(received) + L" / " + (total ? size_text(total) : L"—");
                        std::wstring first = app->trw("size") + L": " + amount;
                        if (state == 2) first += L"   ·   " + app->trw("speed") + L": " +
                            (speed > 0 ? size_text(static_cast<uint64_t>(speed)) + L"/s" : L"—");
                        label(item->hDC, first,
                              RECT{row.left + 358, row.top + 180, row.right - 24, row.top + 206},
                              muted, app->small_font);
                        std::wstring second = app->trw("elapsed") + L": " + time_text(elapsed_seconds);
                        if (state == 2) second += L"   ·   " + app->trw("remaining") + L": " +
                            (total > received && speed > 0 ?
                             time_text(static_cast<uint64_t>((total - received) / speed)) : L"—");
                        label(item->hDC, second,
                              RECT{row.left + 358, row.top + 211, row.right - 24, row.top + 237},
                              muted, app->small_font);
                    }
                    if (!compact && state == 2) {
                        const int start = row.left + 358;
                        const int end = row.right - 24;
                        rounded(item->hDC, RECT{start, row.bottom - 21, end, row.bottom - 13}, pale_blue, 8);
                        const int filled = static_cast<int>((end - start) * std::clamp(percent, 0.0, 100.0) / 100.0);
                        if (filled > 0) rounded(item->hDC,
                            RECT{start, row.bottom - 21, start + filled, row.bottom - 13}, blue, 8);
                    }
                }
                if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &row);
                RestoreDC(item->hDC, saved_clip);
                if (buffer_bitmap) {
                    BitBlt(target_dc, visible.left, visible.top, visible.right - visible.left,
                           visible.bottom - visible.top, buffer_dc, visible.left, visible.top, SRCCOPY);
                    SelectObject(buffer_dc, old_bitmap);
                    DeleteObject(buffer_bitmap);
                }
                if (buffer_dc) DeleteDC(buffer_dc);
                return TRUE;
            }
            break;
        }
        case WM_SIZE:
            app->resize(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case Browse: app->browse_folder(); break;
                case Paste: app->paste_links(); break;
                case Download: app->add(); break;
                case Remove: app->erase(); break;
                case Retry: app->retry_selected(); break;
                case Open: app->open_folder(); break;
                case Logs:
                    cryget::log_event("log.open", "user requested log folder");
                    app->launch_folder(cryget::log_path().parent_path());
                    break;
                case Up: app->move_selected(-1); break;
                case Down: app->move_selected(1); break;
                case Language:
                    if (HIWORD(wparam) == CBN_SELCHANGE) {
                        app->locale = static_cast<size_t>(SendMessageW(app->language, CB_GETCURSEL, 0, 0));
                        app->translate();
                    }
                    break;
                case List:
                    if (HIWORD(wparam) == LBN_SELCHANGE) app->update_actions();
                    break;
            }
            return 0;
        case playlist_message:
            app->finish_playlist();
            return 0;
        case changed_message:
            app->refresh();
            if (wparam) {
                SendMessageW(app->list, LB_SETCURSEL, wparam - 1, 0);
                app->update_actions();
            }
            return 0;
        case progress_message:
            app->progress_pending = false;
            app->invalidate_active_rows();
            return 0;
        case WM_TIMER:
            if (wparam == 1) {
                bool downloading = false;
                const auto now = std::chrono::steady_clock::now();
                {
                    std::lock_guard lock(app->mutex);
                    for (const auto& task : app->tasks) {
                        if (task->state != 2) continue;
                        downloading = true;
                        task->elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - task->started).count();
                        if (now - task->speed_sample >= std::chrono::seconds(2)) task->speed = 0;
                    }
                }
                if (downloading) app->invalidate_active_rows();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            app->shutdown();
            DeleteObject(app->body_font);
            DeleteObject(app->small_font);
            DeleteObject(app->title_font);
            DeleteObject(app->button_font);
            DeleteObject(app->card_font);
            DeleteObject(app->white_brush);
            delete app;
            app = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show) {
    ui_self_test = command_line && std::wstring(command_line) == L"--ui-self-test";
    cryget::log_event("app.start", "Windows WinHTTP" + std::string(ui_self_test ? " UI self-test" : ""));
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.hInstance = instance;
    type.lpszClassName = L"crYGetWindow";
    type.lpfnWndProc = window_proc;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    type.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                16, 16, LR_DEFAULTCOLOR));
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&type);
    HWND window = CreateWindowExW(0, type.lpszClassName, L"crYGet", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1080, 900, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show);
    if (ui_self_test) {
        SetActiveWindow(window);
        wchar_t temporary[MAX_PATH]{};
        const DWORD temporary_length = GetTempPathW(MAX_PATH, temporary);
        bool passed = temporary_length > 0 && temporary_length < MAX_PATH;
        if (passed) {
            SetWindowTextW(app->folder, temporary);
            SendMessageW(app->browse, BM_CLICK, 0, 0);
            passed = app->browse_requests == 1;
        }
        if (passed) {
            const wchar_t* url = L"https://www.youtube.com/watch?v=dQw4w9WgXcQ";
            SendMessageW(app->quality, CB_SETCURSEL, 2, 0);
            SetWindowTextW(app->links, url);
            SendMessageW(app->download, BM_CLICK, 0, 0);
            passed = app->tasks.size() == 1 && SendMessageW(app->list, LB_GETCOUNT, 0, 0) == 1 &&
                     SendMessageW(app->list, LB_GETCURSEL, 0, 0) == 0 &&
                     IsWindowEnabled(app->browse) && IsWindowEnabled(app->paste) &&
                     IsWindowEnabled(app->download) &&
                     IsWindowEnabled(app->logs) && IsWindowEnabled(app->remove) &&
                     IsWindowEnabled(app->open) && !IsWindowEnabled(app->retry) &&
                     !IsWindowEnabled(app->up) && !IsWindowEnabled(app->down) &&
                     control_text(app->links).empty() && app->tasks.front()->quality == "720p" &&
                     std::filesystem::equivalent(app->tasks.front()->folder, temporary);
            if (passed) {
                SendMessageW(app->open, BM_CLICK, 0, 0);
                passed = app->folder_open_requests == 1 &&
                         std::filesystem::equivalent(app->last_opened_folder, temporary);
            }
            if (passed) {
                SendMessageW(app->logs, BM_CLICK, 0, 0);
                passed = app->folder_open_requests == 2 &&
                         app->last_opened_folder == cryget::log_path().parent_path();
            }
            if (passed) {
                SetWindowTextW(app->links, url);
                SendMessageW(app->download, BM_CLICK, 0, 0);
                passed = app->tasks.size() == 1 && control_text(app->links) == url;
            }
            if (passed) {
                const wchar_t* second = L"https://www.youtube.com/watch?v=9bZkp7q19f0";
                SetWindowTextW(app->links, second);
                SendMessageW(app->download, BM_CLICK, 0, 0);
                passed = app->tasks.size() == 2 && SendMessageW(app->list, LB_GETCOUNT, 0, 0) == 2 &&
                         SendMessageW(app->list, LB_GETCURSEL, 0, 0) == 1 &&
                         IsWindowEnabled(app->up) && !IsWindowEnabled(app->down);
            }
            if (passed) {
                const int rebuilds = app->list_rebuilds;
                app->tasks.front()->title = "Updated title";
                app->refresh();
                passed = app->list_rebuilds == rebuilds &&
                         SendMessageW(app->list, LB_GETCURSEL, 0, 0) == 1 &&
                         IsWindowEnabled(app->up) && !IsWindowEnabled(app->down);
            }
            if (passed) {
                SendMessageW(app->list, LB_SETCURSEL, 1, 0);
                SendMessageW(app->up, BM_CLICK, 0, 0);
                passed = app->tasks.front()->url.find("9bZkp7q19f0") != std::string::npos;
            }
            if (passed) {
                SendMessageW(app->list, LB_SETCURSEL, 0, 0);
                app->update_actions();
                passed = IsWindowEnabled(app->down);
                if (passed) SendMessageW(app->down, BM_CLICK, 0, 0);
                passed = passed && app->tasks[1]->url.find("9bZkp7q19f0") != std::string::npos;
            }
            if (passed) {
                SendMessageW(app->list, LB_SETCURSEL, 1, 0);
                app->update_actions();
                SendMessageW(app->up, BM_CLICK, 0, 0);
                passed = app->tasks.front()->url.find("9bZkp7q19f0") != std::string::npos;
            }
            if (passed) {
                app->tasks.front()->state = 4;
                app->refresh();
                SendMessageW(app->list, LB_SETCURSEL, 0, 0);
                app->update_actions();
                passed = IsWindowEnabled(app->retry);
            }
            if (passed) {
                SendMessageW(app->retry, BM_CLICK, 0, 0);
                passed = app->tasks.front()->state == 0 && !IsWindowEnabled(app->retry);
            }
            if (passed) {
                SendMessageW(app->list, LB_SETCURSEL, 0, 0);
                SendMessageW(app->remove, BM_CLICK, 0, 0);
                passed = app->tasks.size() == 1 && SendMessageW(app->list, LB_GETCOUNT, 0, 0) == 1 &&
                         SendMessageW(app->list, LB_GETCURSEL, 0, 0) == 0;
            }
            if (passed) {
                SendMessageW(app->remove, BM_CLICK, 0, 0);
                passed = app->tasks.empty() && SendMessageW(app->list, LB_GETCOUNT, 0, 0) == 0 &&
                         !IsWindowEnabled(app->remove) && !IsWindowEnabled(app->open);
            }
            if (passed) {
                SetWindowTextW(app->links, url);
                SendMessageW(app->download, BM_CLICK, 0, 0);
                app->tasks.front()->state = 4;
                app->refresh();
                SetWindowTextW(app->links, url);
                SendMessageW(app->download, BM_CLICK, 0, 0);
                passed = app->tasks.size() == 2 && app->tasks.front()->state == 4 &&
                         app->tasks.back()->state == 0 && control_text(app->links).empty();
                app->tasks.clear();
                app->refresh();
            }
            if (passed) {
                app->resize(800, 620);
                for (int index = 0; index < 6; ++index) {
                    auto task = std::make_shared<Task>();
                    task->title = "Queue item " + std::to_string(index);
                    app->tasks.push_back(std::move(task));
                }
                app->refresh();
                SendMessageW(app->list, LB_SETCURSEL, 2, 0);
                SendMessageW(app->list, LB_SETTOPINDEX, 2, 0);
                app->refresh();
                RECT viewport{}, row{};
                GetClientRect(app->list, &viewport);
                RECT actions{}, lower_actions{};
                GetWindowRect(app->logs, &actions);
                GetWindowRect(app->up, &lower_actions);
                passed = SendMessageW(app->list, LB_GETCURSEL, 0, 0) == 2 &&
                         SendMessageW(app->list, LB_GETTOPINDEX, 0, 0) == 2 &&
                         SendMessageW(app->list, LB_GETITEMRECT, 2, reinterpret_cast<LPARAM>(&row)) != LB_ERR &&
                         IsWindowVisible(app->list) && viewport.bottom >= 80 &&
                         row.top < viewport.bottom && row.bottom > 0 &&
                         row.bottom - row.top <= viewport.bottom &&
                         lower_actions.top >= actions.bottom;
                if (passed) {
                    app->drawn_rows = 0;
                    InvalidateRect(app->list, nullptr, TRUE);
                    UpdateWindow(app->list);
                    passed = app->drawn_rows > 0 && app->buffered_paints > 0;
                }
            }
        }
        cryget::log_event("ui.self_test", passed ? "passed" : "failed");
        DestroyWindow(window);
        CoUninitialize();
        return passed ? 0 : 1;
    }
    ACCEL shortcut{static_cast<BYTE>(FVIRTKEY | FCONTROL), VK_RETURN, Download};
    HACCEL shortcuts = CreateAcceleratorTableW(&shortcut, 1);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (shortcuts && TranslateAcceleratorW(window, shortcuts, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (shortcuts) DestroyAcceleratorTable(shortcuts);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
#endif
