#pragma once
#include <algorithm>

struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// Shared by drawing and hit testing. Large text reflows instead of shrinking.
struct FormLayout {
    Rect panel, links, paste, add, quality, folder, browse;
    FormLayout(int x, int y, int width, int action_width, int browse_width) {
        constexpr int padding = 32, gap = 24, control = 96;
        const int inner = width - 2 * padding;
        const bool wide = width >= 1600;
        if (wide) {
            const int ax = x + width - padding - action_width;
            links = {x + padding, y + 112, inner - action_width - gap, 264};
            paste = {ax, y + 112, action_width, control};
            add = {ax, y + 280, action_width, control};
        } else {
            links = {x + padding, y + 112, inner, 264};
            const int bw = (inner - gap) / 2;
            paste = {x + padding, y + 400, bw, control};
            add = {paste.x + bw + gap, paste.y, bw, control};
        }
        const int options_y = (wide ? links.y + links.h : add.y + add.h) + 112;
        quality = {x + padding, options_y, std::min(400, inner), control};
        const int folder_y = quality.y + quality.h + 112;
        const int bw = std::min(browse_width, inner / 2);
        if (width >= 1200) {
            browse = {x + width - padding - bw, folder_y, bw, control};
            folder = {x + padding, folder_y, inner - bw - gap, control};
        } else {
            folder = {x + padding, folder_y, inner, control};
            browse = {x + width - padding - bw, folder_y + control + gap, bw, control};
        }
        panel = {x, y, width, browse.y + control + padding - y};
    }
};
