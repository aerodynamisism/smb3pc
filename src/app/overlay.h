// On-screen overlays drawn into a copy of the current picture: the FPS counter
// and the F11 menu. Text uses the in-game dialog font, keeping this independent
// of any text library.
#pragma once

#include <cstdint>

constexpr int kMenuItems = 3;  // fullscreen, FPS counter, widescreen

enum class MenuAction { Up, Down, Select };

struct MenuView {
    int selected = 0;
    bool fullscreen = false;
    bool show_fps = false;
    bool widescreen = false;
};

// Picture to draw on: the 256x240 NES frame, or the 426x240 widescreen compose
// buffer. Overlays live in screen space, so the FPS counter sits in the
// picture's top-left corner and the menu is centred in it either way.
struct OverlayTarget {
    uint32_t* fb;  // first pixel of the picture, R in the lowest byte of each word
    int w, h;      // picture size in pixels
};

// chr: 128 KB CHR ROM holding the game's 2bpp tiles; menu text and icons are
// drawn from the in-game dialog font (CHR banks $5E/$5F).
void draw_fps(OverlayTarget t, int fps, const uint8_t* chr);
void draw_menu(OverlayTarget t, const MenuView& view, const uint8_t* chr);
