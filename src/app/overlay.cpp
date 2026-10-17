#include "app/overlay.h"

#include <cstdio>

namespace {

// Menu colours: NES $0F black panel fill, $36 peach ink (#F0D0B0) for the
// map-border look.
// Colours in the framebuffer layout: R in the lowest byte, opaque alpha in the highest.
constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) {
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}
constexpr uint32_t kBlack = rgb(0x00, 0x00, 0x00);  // NES $0F, text and markers
constexpr uint32_t kSelect = rgb(0xF8, 0x38, 0x00);  // NES $16, selector arrow
constexpr uint32_t kPeach = rgb(0xF0, 0xD0, 0xB0);  // NES $36, panel fill
constexpr uint32_t kPeachDim = rgb(0x78, 0x68, 0x58);  // selected-but-off markers
constexpr uint32_t kPaper = rgb(0xFC, 0xFC, 0xFC);  // NES $30, border trim

void put(OverlayTarget t, int x, int y, uint32_t color) {
    if (x >= 0 && x < t.w && y >= 0 && y < t.h) t.fb[y * t.w + x] = color;
}

void fill_rect(OverlayTarget t, int x, int y, int w, int h, uint32_t color) {
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) put(t, xx, yy, color);
    }
}

// In-game font: SMB3's dialog/status font lives in CHR banks $5E/$5F. The
// dialog message bytes in the disassembly are raw tile indices into the
// pattern page those banks map ($80-$FF), which pins the layout: uppercase
// A-P at (5E,48-63), Q-X at (5F,0-7), Y/Z at (5F,8-9), digits at (5F,48-57)
// (e.g. O=$BE -> (5E,62), T=$C3 -> (5F,3), 0-9 verified on proof sheets).
// The on/off marker is a solid block tile; off is an empty slot.
struct FontTile {
    uint8_t bank;
    uint8_t tile;
};

bool font_tile(char c, FontTile& t) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c >= 'A' && c <= 'P') {
        t = FontTile{0x5E, static_cast<uint8_t>(48 + (c - 'A'))};
        return true;
    }
    if (c >= 'Q' && c <= 'X') {
        t = FontTile{0x5F, static_cast<uint8_t>(c - 'Q')};
        return true;
    }
    if (c == 'Y') {
        t = FontTile{0x5F, 8};
        return true;
    }
    if (c == 'Z') {
        t = FontTile{0x5F, 9};
        return true;
    }
    if (c >= '0' && c <= '9') {
        t = FontTile{0x5F, static_cast<uint8_t>(48 + (c - '0'))};
        return true;
    }
    return false;  // space and anything else is blank
}

// Blits one 8x8 2bpp CHR tile. Value 3 is transparent (the fill behind shows
// through), unless opaque is set, which paints it black instead — used by the
// border so no tan shows outside its white lines. Text uses the outline form
// (values 0/2), which keeps digits readable; the border uses the solid form
// (any nonzero value).
void draw_tile(OverlayTarget t, int x, int y, const uint8_t* chr, FontTile ft, uint32_t color,
               bool solid, bool flip = false, bool opaque = false) {
    const uint8_t* g = chr + static_cast<size_t>(ft.bank) * 1024 + static_cast<size_t>(ft.tile) * 16;
    for (int row = 0; row < 8; row++) {
        const uint8_t lo = g[row];
        const uint8_t hi = g[row + 8];
        for (int col = 0; col < 8; col++) {
            const int sc = flip ? col : 7 - col;
            const unsigned v = (((hi >> sc) & 1) << 1) | ((lo >> sc) & 1);
            if (v == 3 || (!solid && v == 1)) {
                if (opaque) put(t, x + col, y + row, kBlack);
                continue;
            }
            put(t, x + col, y + row, color);
        }
    }
}


constexpr int kAdvance = 9;  // 8px glyphs plus 1px so stroked neighbours never touch

void draw_gametext(OverlayTarget t, int x, int y, const char* text, const uint8_t* chr) {
    // Two passes for the HUD look: white cores with a 1px black rim, the
    // same stroke weight as the in-game HUD glyphs.
    for (int pass = 0; pass < 2; pass++) {
        int gx = x;
        for (const char* c = text; *c != '\0'; c++, gx += kAdvance) {
            FontTile ft{0, 0};
            if (!font_tile(*c, ft)) continue;
            if (pass == 0) {
                const uint8_t* g =
                    chr + static_cast<size_t>(ft.bank) * 1024 + static_cast<size_t>(ft.tile) * 16;
                for (int row = 0; row < 8; row++) {
                    const uint8_t lo = g[row];
                    const uint8_t hi = g[row + 8];
                    for (int col = 0; col < 8; col++) {
                        const unsigned v = (((hi >> (7 - col)) & 1) << 1) | ((lo >> (7 - col)) & 1);
                        if (v == 3 || v == 1) continue;
                        for (int dy = -1; dy <= 1; dy++) {
                            for (int dx = -1; dx <= 1; dx++)
                                put(t, gx + col + dx, y + row + dy, kBlack);
                        }
                    }
                }
            } else {
                draw_tile(t, gx, y, chr, ft, kPaper, false);
            }
        }
    }
}

int gametext_width(const char* text) {
    int n = 0;
    for (const char* c = text; *c != '\0'; c++) n++;
    return n * kAdvance;
}

void put_mark(OverlayTarget t, int x, int y, uint32_t color) {
    put(t, x, y, color);
    put(t, x + 1, y, color);
}

// Check mark in an 8x8 cell: short arm down-right, long arm up-right.
void draw_check(OverlayTarget t, int x, int y, uint32_t color) {
    for (int i = 0; i < 3; i++) {
        put_mark(t, x + 1 + i, y + 4 + i, color);
    }
    for (int i = 0; i < 4; i++) {
        put_mark(t, x + 3 + i, y + 6 - i, color);
    }
}

// Cross mark in an 8x8 cell: two diagonals.
void draw_cross(OverlayTarget t, int x, int y, uint32_t color) {
    for (int i = 0; i < 6; i++) {
        put_mark(t, x + 1 + i, y + 1 + i, color);
        put_mark(t, x + 5 - i, y + 1 + i, color);
    }
}

// Cursor: filled right-pointing triangle with a 1px black rim. (The two
// arrow-like tiles at (5F,2E)/(5F,2F) are halves of one 16px map object, so
// neither works alone in the 8px cursor cell.)
void draw_cursor(OverlayTarget t, int x, int y) {
    for (int row = 0; row < 8; row++) {
        const int half = row < 4 ? row : 7 - row;
        for (int col = 0; col <= half + 1; col++) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) put(t, x + col + dx, y + row + dy, kBlack);
            }
        }
    }
    for (int row = 0; row < 8; row++) {
        const int half = row < 4 ? row : 7 - row;
        for (int col = 0; col <= half + 1; col++) {
            put(t, x + col, y + row, kSelect);
        }
    }
}

}  // namespace

void draw_fps(OverlayTarget t, int fps, const uint8_t* chr) {
    char text[16];
    std::snprintf(text, sizeof text, "FPS %d", fps);
    // In the picture's top-left corner: inside the 4:3 overscan crop, and on the
    // added left columns in widescreen. No backing box: the stroked glyphs stay
    // readable over gameplay on their own.
    draw_gametext(t, 12, 11, text, chr);
}

void draw_menu(OverlayTarget t, const MenuView& view, const uint8_t* chr) {
    // Tile-aligned black panel framed with the world-map status strip border
    // (CHR bank $5E on the $0000 page). The right-side corner tiles read as
    // proper corners, so the left side mirrors them with a horizontal flip.
    // Centred in the picture, so it lands in the middle of the NES frame in
    // 4:3 and of the wide window in widescreen.
    constexpr int w = 192, h = 88;
    const int x0 = (t.w - w) / 2, y0 = 56;
    constexpr FontTile kTop{0x5E, 0x21}, kBottom{0x5E, 0x24};
    constexpr FontTile kLeft{0x5E, 0x26}, kRight{0x5E, 0x27};
    constexpr FontTile kTopCorner{0x5E, 0x22}, kBottomCorner{0x5E, 0x25};
    auto edge = [&](int x, int y, FontTile ft) { draw_tile(t, x, y, chr, ft, kPaper, false, false, true); };
    // Full-box black backing first: border tiles leave transparent pixels that
    // must fall back to black, never the game frame behind the menu.
    fill_rect(t, x0, y0, w, h, kPeach);
    for (int x = x0 + 8; x < x0 + w - 8; x += 8) {
        edge(x, y0, kTop);
        edge(x, y0 + h - 8, kBottom);
    }
    for (int y = y0 + 8; y < y0 + h - 8; y += 8) {
        edge(x0, y, kLeft);
        edge(x0 + w - 8, y, kRight);
    }
    draw_tile(t, x0, y0, chr, kTopCorner, kPaper, false, true, true);
    draw_tile(t, x0 + w - 8, y0, chr, kTopCorner, kPaper, false, false, true);
    draw_tile(t, x0, y0 + h - 8, chr, kBottomCorner, kPaper, false, true, true);
    draw_tile(t, x0 + w - 8, y0 + h - 8, chr, kBottomCorner, kPaper, false, false, true);
    constexpr int label_x = 20;
    constexpr int cursor_x = 10;
    constexpr int icon_x = 126;

    draw_gametext(t, x0 + (w - gametext_width("SETTINGS")) / 2, y0 + 10, "Settings", chr);

    const char* labels[kMenuItems] = {"FULLSCREEN", "FPS COUNTER", "WIDESCREEN"};
    const int ys[kMenuItems] = {y0 + 26, y0 + 40, y0 + 54};
    for (int i = 0; i < kMenuItems; i++) {
        draw_gametext(t, x0 + label_x, ys[i], labels[i], chr);
    }
    draw_cursor(t, x0 + cursor_x, ys[view.selected] - 1);
    // Check when on, cross when selected-but-off so state stays distinguishable.
    const bool on[kMenuItems] = {view.fullscreen, view.show_fps, view.widescreen};
    for (int i = 0; i < kMenuItems; i++) {
        if (on[i]) {
            draw_check(t, x0 + icon_x, ys[i], kBlack);
        } else if (view.selected == i) {
            draw_cross(t, x0 + icon_x, ys[i], kPeachDim);
        }
    }
}
