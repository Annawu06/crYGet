# crYGet 2026.10.08.9

## Changes

- Avoid a second YouTube video inspection from the thumbnail worker while a download task is inspecting the same video.
- Retry interrupted Windows media ranges from the last written byte when possible; limit retries and preserve cancellation.
- Explain when every submitted link is already in the Windows queue, and retain those links in the input box so the user can edit them.
- Update the Windows task list outside the task lock to avoid UI stalls during owner-drawn row updates.
- Keep the Windows task list at the same scroll position when progress and thumbnail updates refresh it.
- Keep Windows download cards visible in shorter windows, and select the existing card when a link is submitted twice.
- Reflow the Windows input panel when the window is short, keep thumbnail proportions, and preserve the visible queue row while resizing.
- Clip Windows task card drawing to the list viewport so a partially visible next thumbnail cannot cover the bottom buttons.
- Keep the list viewport clip when drawing rounded thumbnails inside a card.
- Use Windows automatic proxy settings for WinHTTP requests, retry transient name resolution and request timeouts, and record the failing host in logs.
- Report total media size from the first range response, show received bytes while the total is unknown, and use smaller Windows ranges so progress remains visible on slow connections.
- Select newly added Windows queue items automatically and keep a neighboring item selected after removal, so consecutive clicks on Remove work without reselecting a card.
- Allow the Delete key to remove the selected Windows queue item.
- Disable Retry, Open folder, Remove, and move buttons when their action is unavailable; draw disabled buttons distinctly and check their states in the Windows UI self-test.
- Show both UTC and local time on diagnostic log lines to make Windows reports easier to match to the clock.
- Add installed and portable Windows UI checks that click Download, change quality, reorder tasks, retry, and remove tasks; verify silent uninstall.
- Read YouTube Mix (`RD…`) playlists from their watch page and load subsequent Mix entries through the next endpoint. Ordinary playlist browse responses do not contain these generated lists.
- Load thumbnails for playlist videos on Linux and Windows without making a separate video details request for every playlist entry.
- When the watch page has no usable MP4 stream, try Android and embedded player clients before the web client, with matching client headers and user agents.
- Show the player response error when all clients fail, so a playback restriction is not reported as missing MP4 URLs.
- Retain background playlist expansion, drag priority, and in-process MP4 audio/video merging.

## Packages

- `crYGet-Setup-2026.10.08.9-windows-x64.exe`
- `crYGet-2026.10.08.9-windows-x64-portable.zip`
- `cryget_2026.10.08.9+debian.forky_amd64.deb`
