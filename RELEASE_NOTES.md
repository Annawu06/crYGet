# crYGet 2026.10.08

## Changes

- Replace QuickJS with embedded V8 for YouTube player JavaScript.
- Merge separate audio and video streams in-process with FFmpeg libraries; no external FFmpeg executable is required.
- Expand public YouTube playlists into queued downloads on Windows and Linux.
- Update the Linux build baseline to Debian 13.

## Packages

- `crYGet-Setup-2026.10.08-windows-x64.exe`
- `cryget_2026.10.08+debian13_amd64.deb`
- `cryget_2026.10.08+debian.forky_amd64.deb` when built on Debian Forky

The Windows installer must be built with an LGPL-compatible FFmpeg MinGW package and the exact corresponding FFmpeg source. Include the required third-party license notices and source archive in the installer.
