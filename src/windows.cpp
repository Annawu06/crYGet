#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
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
#include "video_core.hpp"
#include "video_links.hpp"
#include "winhttp_download.hpp"

namespace {
constexpr UINT changed_message = WM_APP + 1;
constexpr UINT progress_message = WM_APP + 2;
enum Control { Links = 101, Folder, Browse, Quality, Download, List, Remove, Retry, Open, Up, Down, Language, Logs };

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
    ~Task() { if (thumbnail) DeleteObject(thumbnail); }
};

struct App {
    HWND window = nullptr;
    HWND links = nullptr, folder = nullptr, browse = nullptr, quality = nullptr;
    HWND download = nullptr, list = nullptr, remove = nullptr, retry = nullptr, open = nullptr;
    HWND up = nullptr, down = nullptr, language = nullptr, logs = nullptr, heading = nullptr, save_label = nullptr, empty = nullptr;
    std::vector<std::shared_ptr<Task>> tasks;
    std::vector<std::shared_ptr<Task>> active;
    std::deque<std::shared_ptr<Task>> preview_queue;
    std::vector<std::thread> preview_workers;
    std::mutex mutex;
    std::condition_variable stopped;
    std::condition_variable preview_ready;
    std::atomic<bool> closing{false};
    std::atomic<bool> progress_pending{false};
    size_t locale = 0;
    HFONT body_font = nullptr, small_font = nullptr, title_font = nullptr, button_font = nullptr, card_font = nullptr;
    HBRUSH white_brush = nullptr;

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
            if (!task->stop && !closing) {
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

    void schedule() {
        while (!closing && active.size() < 2) {
            auto found = std::find_if(tasks.begin(), tasks.end(), [](const auto& task) { return task->state == 0; });
            if (found == tasks.end()) break;
            auto task = *found;
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
                task->saved_path = output.u8string();
                task->saved_size = std::filesystem::file_size(output);
                task->state = 3;
            }
        } catch (const std::exception& error) {
            cryget::log_event("queue.error", task->url + " " + error.what());
            std::lock_guard lock(mutex);
            task->error = error.what();
            task->state = task->stop ? 5 : 4;
        }
        {
            std::lock_guard lock(mutex);
            active.erase(std::remove(active.begin(), active.end(), task), active.end());
            schedule();
            stopped.notify_all();
        }
    }

    std::shared_ptr<Task> selected() {
        const auto index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        std::lock_guard lock(mutex);
        return index >= 0 && static_cast<size_t>(index) < tasks.size() ? tasks[index] : nullptr;
    }

    void refresh() {
        const int selection = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        {
            std::lock_guard lock(mutex);
            for (const auto& task : tasks) {
                const char* key = task->state == 0 ? "queued" : task->state == 1 ? "connecting" :
                                  task->state == 2 ? "downloading" : task->state == 3 ? "complete" :
                                  task->state == 4 ? "failed" : "canceling";
                auto label = wide(task->title.empty() ? task->url : task->title);
                label += L"  —  " + trw(key);
                if (task->state == 2) label += L" " + std::to_wstring(static_cast<int>(task->percent)) + L"%";
                if (task->state == 4) label += L": " + wide(task->error);
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
        }
        if (selection >= 0) SendMessageW(list, LB_SETCURSEL, selection, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        ShowWindow(empty, SendMessageW(list, LB_GETCOUNT, 0, 0) == 0 ? SW_SHOW : SW_HIDE);
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
        SetWindowTextW(download, trw("add").c_str());
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
        std::filesystem::path target;
        try { target = cryget::checked_folder(utf8(control_text(folder))); }
        catch (const std::exception& error) {
            cryget::log_event("queue.folder_error", error.what());
            MessageBoxW(window, wide(error.what()).c_str(), L"crYGet", MB_ICONERROR); return;
        }
        const auto input = utf8(control_text(links));
        const auto urls = cryget::parse_links(input);
        if (urls.empty()) {
            MessageBoxW(window, trw("invalid").c_str(), L"crYGet", MB_ICONERROR);
            return;
        }
        const int quality_index = static_cast<int>(SendMessageW(quality, CB_GETCURSEL, 0, 0));
        const std::string quality_value = quality_index == 1 ? "1080p" : quality_index == 2 ? "720p" : "Best";
        {
            std::lock_guard lock(mutex);
            for (const auto& url : urls) {
                if (std::any_of(tasks.begin(), tasks.end(), [&](const auto& task) { return task->url == url; })) continue;
                auto task = std::make_shared<Task>();
                task->url = url; task->folder = target; task->quality = quality_value;
                tasks.push_back(task);
                preview_queue.push_back(task);
            }
            preview_ready.notify_all();
            schedule();
        }
        SetWindowTextW(links, L"");
        refresh();
    }

    void erase() {
        auto task = selected();
        if (!task) return;
        {
            std::lock_guard lock(mutex);
            task->stop = true;
            tasks.erase(std::remove(tasks.begin(), tasks.end(), task), tasks.end());
            cryget::log_event("queue.remove", task->url);
            schedule();
        }
        refresh();
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
        refresh_without_lock(next);
    }

    void refresh_without_lock(ptrdiff_t selection) {
        // Keep UI changes on the window thread after releasing the task lock.
        PostMessageW(window, changed_message, static_cast<WPARAM>(selection + 1), 0);
    }

    void open_folder() {
        auto task = selected();
        if (!task) return;
        ShellExecuteW(window, L"open", task->folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void browse_folder() {
        BROWSEINFOW info{};
        info.hwndOwner = window;
        const auto title = trw("choose_folder");
        info.lpszTitle = title.c_str();
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        auto* chosen = SHBrowseForFolderW(&info);
        if (!chosen) return;
        wchar_t path[MAX_PATH]{};
        if (SHGetPathFromIDListW(chosen, path)) SetWindowTextW(folder, path);
        CoTaskMemFree(chosen);
    }

    void resize(int width, int height) {
        const int margin = 28, gap = 12;
        const int content = width - 2 * margin;
        MoveWindow(language, width - 226, 32, 188, 240, TRUE);
        MoveWindow(heading, 60, 150, content - 64, 24, TRUE);
        MoveWindow(links, 60, 179, content - 64, 76, TRUE);
        MoveWindow(save_label, 260, 267, content - 264, 22, TRUE);
        MoveWindow(quality, 60, 292, 180, 240, TRUE);
        MoveWindow(folder, 260, 292, width - 624, 38, TRUE);
        MoveWindow(browse, width - 352, 292, 102, 38, TRUE);
        MoveWindow(download, width - 232, 289, 172, 44, TRUE);
        MoveWindow(list, 60, 425, content - 64, std::max(100, height - 515), TRUE);
        MoveWindow(empty, 88, 450, content - 120, std::max(54, height - 568), TRUE);
        const int bottom = height - 70;
        HDC dc = GetDC(window);
        HGDIOBJ old_font = SelectObject(dc, button_font);
        int button_x = 60;
        for (const auto& button : {std::pair<HWND, int>{remove, 140}, {retry, 140},
                                   {open, 140}, {logs, 160}}) {
            const auto caption = control_text(button.first);
            SIZE extent{};
            GetTextExtentPoint32W(dc, caption.c_str(), static_cast<int>(caption.size()), &extent);
            const int button_width = std::max(button.second, static_cast<int>(extent.cx) + 32);
            MoveWindow(button.first, button_x, bottom, button_width, 38, TRUE);
            button_x += button_width + gap;
        }
        SelectObject(dc, old_font);
        ReleaseDC(window, dc);
        MoveWindow(up, width - 150, bottom, 38, 38, TRUE);
        MoveWindow(down, width - 100, bottom, 38, 38, TRUE);
        InvalidateRect(window, nullptr, TRUE);
    }

    void shutdown() {
        std::unique_lock lock(mutex);
        closing = true;
        for (const auto& task : tasks) task->stop = true;
        for (const auto& task : active) task->stop = true;
        preview_queue.clear();
        preview_ready.notify_all();
        stopped.wait(lock, [&] { return active.empty(); });
        lock.unlock();
        for (auto& worker : preview_workers) if (worker.joinable()) worker.join();
        window = nullptr;
    }
};

App* app = nullptr;

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
        app->list = control(window, L"LISTBOX", L"", WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, List);
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
        app->start_previews();
        SetTimer(window, 1, 1000, nullptr);
        return 0;
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {1040, 760};
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
            rounded(dc, RECT{28, 132, client.right - 28, 350}, white, 20);
            rounded_outline(dc, RECT{28, 132, client.right - 28, 350}, line, 20);
            label(dc, app->trw("quality"), RECT{60, 266, 240, 289}, muted, app->small_font);
            rounded(dc, RECT{28, 370, client.right - 28, client.bottom - 16}, white, 20);
            rounded_outline(dc, RECT{28, 370, client.right - 28, client.bottom - 16}, line, 20);
            label(dc, app->trw("library"), RECT{60, 383, client.right - 60, 417}, ink, app->card_font);
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
                const COLORREF background = primary ? (pressed ? RGB(38, 72, 151) : blue) :
                                            item->CtlID == Remove ? (pressed ? RGB(249, 218, 211) : peach) :
                                            item->CtlID == Browse ? (pressed ? RGB(211, 234, 221) : mint) :
                                            pressed ? lavender : neutral;
                rounded(item->hDC, item->rcItem, background, 12);
                if (!primary) rounded_outline(item->hDC, item->rcItem, line, 12);
                wchar_t name[160]{};
                GetWindowTextW(item->hwndItem, name, 160);
                RECT r = item->rcItem;
                if (pressed) OffsetRect(&r, 0, 1);
                label(item->hDC, name, r, primary ? white : ink, app->button_font,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (item->itemState & ODS_FOCUS) {
                    RECT focus = item->rcItem;
                    InflateRect(&focus, -4, -4);
                    DrawFocusRect(item->hDC, &focus);
                }
                return TRUE;
            }
            if (item->CtlID == List && item->itemID != static_cast<UINT>(-1)) {
                RECT row = item->rcItem;
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
                    const RECT preview{row.left + 16, row.top + 16, row.left + 336, row.top + 196};
                    rounded(item->hDC, preview, pale_blue, 10);
                    rounded_outline(item->hDC, preview, line, 10);
                    if (thumbnail && thumbnail_width > 0 && thumbnail_height > 0) {
                        HDC source = CreateCompatibleDC(item->hDC);
                        if (source) {
                            HGDIOBJ old = SelectObject(source, thumbnail);
                            const int saved = SaveDC(item->hDC);
                            HRGN clip = CreateRoundRectRgn(preview.left, preview.top, preview.right + 1, preview.bottom + 1, 10, 10);
                            SelectClipRgn(item->hDC, clip);
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
                    const COLORREF state_color = state == 3 ? RGB(40, 112, 91) : state == 4 ? RGB(168, 63, 72) : blue;
                    label(item->hDC, wide(title.empty() ? url : title),
                          RECT{row.left + 358, row.top + 30, row.right - 24, row.top + 115}, ink, app->card_font,
                          DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
                    const char* key = state == 0 ? "queued" : state == 1 ? "connecting" : state == 2 ? "downloading" :
                                      state == 3 ? "complete" : state == 4 ? "failed" : "canceling";
                    std::wstring status = app->trw(key);
                    if (state == 2) status += L"  " + std::to_wstring(static_cast<int>(percent)) + L"%";
                    if (state == 4 && !error.empty()) status += L" · " + wide(error);
                    const RECT status_box{row.left + 358, row.top + 133, row.right - 24, row.top + 172};
                    rounded(item->hDC, status_box, state == 3 ? mint : state == 4 ? peach : pale_blue, 8);
                    label(item->hDC, status, RECT{status_box.left + 12, status_box.top + 2,
                          status_box.right - 12, status_box.bottom - 2}, state_color, app->small_font);
                    if (state == 2 || state == 3) {
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
                    if (state == 2) {
                        const int start = row.left + 358;
                        const int end = row.right - 24;
                        rounded(item->hDC, RECT{start, row.bottom - 21, end, row.bottom - 13}, pale_blue, 8);
                        const int filled = static_cast<int>((end - start) * std::clamp(percent, 0.0, 100.0) / 100.0);
                        if (filled > 0) rounded(item->hDC,
                            RECT{start, row.bottom - 21, start + filled, row.bottom - 13}, blue, 8);
                    }
                }
                if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &row);
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
                case Download: app->add(); break;
                case Remove: app->erase(); break;
                case Retry: app->retry_selected(); break;
                case Open: app->open_folder(); break;
                case Logs:
                    cryget::log_event("log.open", "user requested log folder");
                    ShellExecuteW(window, L"open", cryget::log_path().parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                case Up: app->move_selected(-1); break;
                case Down: app->move_selected(1); break;
                case Language:
                    if (HIWORD(wparam) == CBN_SELCHANGE) {
                        app->locale = static_cast<size_t>(SendMessageW(app->language, CB_GETCURSEL, 0, 0));
                        app->translate();
                    }
                    break;
            }
            return 0;
        case changed_message:
            app->refresh();
            if (wparam) SendMessageW(app->list, LB_SETCURSEL, wparam - 1, 0);
            return 0;
        case progress_message:
            app->progress_pending = false;
            InvalidateRect(app->list, nullptr, FALSE);
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
                if (downloading) InvalidateRect(app->list, nullptr, FALSE);
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    cryget::log_event("app.start", "Windows WinHTTP");
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
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
#endif
