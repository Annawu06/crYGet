# crYGet 2026.10.10 for Debian Forky

This release contains the latest Linux desktop build. It is packaged for Debian Forky/Sid x86-64 and requires the distribution's Node 24 embedding library (`libnode137`).

## Changes

- Expand YouTube Mix playlists and load playlist thumbnails without inspecting every entry separately.
- Try Android and embedded player responses when the watch page has no usable MP4 stream, and show the player error when no client succeeds.
- Keep playlist expansion in the background and retain download priority controls and in-process MP4 merging.
- Include updated language strings and diagnostic timestamps with both UTC and local time.

## Package

- `cryget_2026.10.10+debian.forky_amd64.deb`

Install on Debian Forky with `sudo apt install ./cryget_2026.10.10+debian.forky_amd64.deb` so APT resolves dependencies. This package is not compatible with Debian 13 Trixie, which provides `libnode115`, or Ubuntu 22.04, whose Node library is too old for this build. Do not add Forky repositories to a Trixie system just to install this package.
