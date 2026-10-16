// On-screen overlays drawn into a copy of the 256x240 NES frame: the FPS counter
// and the F11 menu. Text uses the in-game dialog font, keeping this independent
// of any text library.
#pragma once

#include <cstdint>

constexpr int kMenuItems = 2;  // fullscreen, FPS counter

enum class MenuAction { Up, Down, Select };

struct MenuView {
    int selected = 0;
    bool fullscreen = false;
    bool show_fps = false;
};

// fb: 256 * 240 RGBA words, R in the lowest byte.
// chr: 128 KB CHR ROM holding the game's 2bpp tiles; menu text and icons are
// drawn from the in-game dialog font (CHR banks $5E/$5F).
void draw_fps(uint32_t* fb, int fps, const uint8_t* chr);
void draw_menu(uint32_t* fb, const MenuView& view, const uint8_t* chr);
