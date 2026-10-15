// On-screen overlays drawn into a copy of the 256x240 NES frame: the FPS counter and the
// F11 menu. A 5x7 bitmap font keeps this independent of any text library.
#pragma once

#include <cstdint>

constexpr int kMenuItems = 3;  // fullscreen, FPS counter, resume

enum class MenuAction { Up, Down, Select };

struct MenuView {
    int selected = 0;
    bool fullscreen = false;
    bool show_fps = false;
};

// fb: 256 * 240 RGBA words, R in the lowest byte.
void draw_fps(uint32_t* fb, int fps);
void draw_menu(uint32_t* fb, const MenuView& view);
