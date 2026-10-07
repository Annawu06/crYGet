# crYGet

crYGet is a desktop application for downloading YouTube videos. It is written in C++17 and provides a graphical interface for Windows and Linux. The application runs without Python, yt-dlp, or a browser extension.

## Get crYGet

Download the installer for your system from [Releases](https://github.com/Annawu06/crYGet/releases):

| System                  | Package                           |
| ----------------------- | --------------------------------- |
| Windows 64-bit          | `crYGet-Setup-*-windows-x64.exe`  |
| Ubuntu 22.04 x86-64     | `cryget_*+ubuntu22.04_amd64.deb`  |
| Debian forky/sid x86-64 | `cryget_*+debian.forky_amd64.deb` |

On Windows, run the installer and launch crYGet from the Start menu. On Linux, install the matching `.deb` package and launch crYGet from the application menu. The Linux packages install their required system libraries through the package manager.

Some videos have separate audio and video streams. Install **FFmpeg** if you want crYGet to combine them. A combined MP4 stream does not need FFmpeg.

## Features

### Downloads

- Add up to 100 YouTube links at a time; downloads beyond the two active slots are queued.
- Choose the best available quality, 1080p, or 720p, and select a save folder.
- View download progress, speed, and estimated remaining time.
- Reorder queued downloads, retry failed downloads, and open the output folder from a download card.

### Desktop experience

- Use a native Windows or Linux desktop interface.
- Switch between English, Simplified Chinese, Traditional Chinese, French, Russian, Spanish, and Portuguese interface variants.
- Open the diagnostic log directory from the application when investigating an error.

### Playback URL handling

- Read video information and MP4 stream URLs from YouTube responses.
- Use the bundled QuickJS engine to handle supported player signature and `n` parameter transformations.
- Download media without running Python, yt-dlp, or ejs.

## Use crYGet

1. Paste one or more YouTube video URLs into the link field.
2. Choose a quality setting and save folder.
3. Select **Download Video**. Additional videos will wait in the queue.

## Build from source

### Linux

Install a C++17 compiler, Make, `pkg-config`, and development packages for X11, Xft, Fontconfig, libcurl, and libjpeg. Then run:

```sh
make
./cryget-desktop
```

### Windows

Build with Visual Studio's C++ workload and CMake:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
.\build\Release\cryget-desktop.exe
```

The repository also provides `scripts/build-windows.sh` for cross-compilation with MinGW-w64 and NSIS, and `scripts/build-release.sh` for building the release installers.

Run the automated tests on Linux with `make test`.

## Current scope

crYGet supports MP4 streams that YouTube exposes to its player. Some videos may be unavailable, including those that require special authorization, use DRM, or rely on player behavior that crYGet does not yet support. YouTube player changes can also affect downloads.

## Reporting issues

When reporting a problem, include your operating system, the steps to reproduce it, and the error shown by the application. Use **Open Log Directory** to find the diagnostic log. Check the log for local file paths before sharing it.

## Third-party components

crYGet bundles [QuickJS](https://bellard.org/quickjs/) and [Acorn](https://github.com/acornjs/acorn) for JavaScript processing. Their license notices are included with the installers and in `third_party/` in the source tree.
