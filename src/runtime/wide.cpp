// Widescreen (16:9) compose pass.
//
// The 256x240 frame the emulator renders stays byte-for-byte what the NES would show:
// this pass runs after run_frame(), at the same vblank moment as hud_compose(), and only
// reads game state (RAM, sprite RAM, Tile_Mem, the per-scanline PPU snapshots and the PRG
// ROM's level tables). It copies that frame into a 426x240 picture and fills the 85
// columns on either side with the level content that is really there. Game logic,
// collision, camera, spawning and activation are never touched by any of it.
//
// Model: the frame's left edge sits at world X S (Horz_Scroll) and the frame is 256 world
// pixels wide; the wide picture widens that window to 426. While the window fits inside
// the level the frame stays centred and the extra 85 columns show real level columns to
// either side. At either end of the level the window stops (the frame slides off-centre
// instead) so nothing outside the level is ever invented. Levels shorter than the wide
// window keep the frame centred on black, as do non-level screens (title, map, ending).
// Pixels with no level row there show the row's backdrop colour, exactly the colour those
// pixels would have with no tile in them.
//
// Sources, one per element:
//   - Background columns: Tile_Mem ($6000), which the level loader filled with the whole
//     level and block changes update. A block byte's bits 6-7 are its palette; the low
//     byte indexes TileLayout_ByTileset[tileset]'s four 256-byte pages (top-left,
//     bottom-left, top-right, bottom-right), whose bytes are nametable tile numbers, and
//     the tile's pattern comes from the CHR window the game had banked for that scanline.
//   - The page, palette, backdrop and CHR banking of each scanline come from hud_lines,
//     the same per-line snapshots the split compositor redraws rows from, so extra columns
//     shaded or bank-switched by autoscroll effects match the visible ones. The rows the
//     frame gives to the status bar (hud_top..239) continue the playfield downward, drawn
//     with the last playfield row's palette and banking, so the level's ground runs on
//     beside the bar; the bar's own nametable holds the HUD, not level content.
//   - Sprites: OAM, at their true world position. Each object owns a six-sprite region
//     (Object_SprRAM) that the drawers write screen-relative coordinates into; the same
//     8-bit values the frame draws are lifted to the 256-pixel page nearest the object,
//     which recovers their place in the wider window. Pieces that are vertically hidden
//     (Y >= 239, including the game's $F8 blanks) or whose lifted spot is nowhere near
//     the object do not belong to it and are left out instead of being drawn somewhere
//     invented.
//
// Known differences from a hypothetical 426-pixel NES running SMB3, kept deliberately:
//   - Objects are spawned at world S+272 (the frame's right edge), so an enemy that
//     exists only beyond that is not shown until the game activates it; the outer ~70
//     columns on the right can look empty until it enters. Wide mode never moves
//     activation earlier, because that would change gameplay.
//   - The 8-sprites-per-line hardware limit and the sprite-0 hit apply to the frame the
//     game drew; the extra columns can show more sprites on a line than a real PPU would.
//   - Sprites parked in the shared spare slots ($00-$27) are screen-space effects with no
//     world anchor (score popups, object overflow spares) and are not repeated outside
//     the frame.
//   - Camera animation that the game scripts per frame (autoscroll pushes, pipe
//     transitions, the level-end walk) moves the frame exactly as the NES does; the wide
//     window follows it rather than forcing the player back to centre.

#include "runtime/machine.h"

#include <algorithm>
#include <cstring>

#include "runtime/nes_palette.h"
#include "smb3_syms.h"

namespace {

constexpr int kFrameW = 256;
constexpr int kRows = 240;
constexpr int kScreens = 15;                     // Tile_Mem holds 15 screens
constexpr int kScreenBytes = 0x1B0;              // 16 columns x 27 rows of blocks
constexpr int kBlockRows = 27;
constexpr int kLevelHeightPx = kBlockRows * 16;  // 432

// Level tilesets whose blocks have the standard four-page layout table (the file header
// explains the format). 0 is the world map and 15+ are the bonus games, not levels.
constexpr int kFirstTileset = 1;
constexpr int kLastTileset = 14;                 // 14 = underground, same mechanism

// Six sprites per object region, eight object slots plus the player.
constexpr int kMaxPieces = (8 + 1) * 6;

uint8_t reverse_bits(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) r = static_cast<uint8_t>((r << 1) | ((b >> i) & 1));
    return r;
}

inline int mem16(const Machine& m, uint16_t lo, uint16_t hi) {
    return m.ram[lo & 0x7FF] | (static_cast<int>(m.ram[hi & 0x7FF]) << 8);
}

// World Y carried by a captured scanline's loopback scroll: the vertical nametable picks
// the second 240-row screen, matching the per-dot pass's bump_vert and hud_compose.
inline int world_y_of(uint16_t v) {
    return static_cast<int>(((v >> 11) & 1) * 240 + ((v >> 5) & 0x1F) * 8 + ((v >> 12) & 7));
}

}  // namespace

void Machine::set_widescreen(bool on) {
    wide_enabled = on;
    if (on && wide_fb.size() != static_cast<size_t>(kWideW) * kRows) {
        wide_fb.assign(static_cast<size_t>(kWideW) * kRows, 0xFF000000u);
    }
}

void Machine::wide_compose() {
    if (!wide_enabled || wide_fb.size() != static_cast<size_t>(kWideW) * kRows) return;
    std::fill(wide_fb.begin(), wide_fb.end(), 0xFF000000u);

    // A wide level frame: a horizontally scrolling level with the standard block layout.
    // World_EnterState is 3 exactly while inside a level and clears on the way back to the
    // map, title or ending, so the wide window never applies to those.
    const uint8_t enter_state = rd(smb3::World_EnterState);
    const uint8_t tileset = rd(smb3::Level_Tileset);
    const bool level_frame = enter_state == 3 && rd(smb3::Level_7Vertical) == 0 &&
                             tileset >= kFirstTileset && tileset <= kLastTileset;
    // Level_Width is the last valid screen index (object tables are indexed up to it, and
    // the camera's screen high byte may equal it), so the level is one screen longer.
    const int screens = std::clamp(static_cast<int>(rd(smb3::Level_Width)) + 1, 1, kScreens);
    const int level_px = screens * kFrameW;
    const int cam_x = mem16(*this, smb3::Horz_Scroll, smb3::Horz_Scroll_Hi);

    // Place the wide window around the frame: centred while it fits, stopped at the level
    // ends (the frame slides toward the other edge). Frame left = cam - left stays inside
    // [0, 170] by construction, so the frame is never clipped by the window.
    bool window = level_frame && level_px >= kWideW;
    int left = 0;
    if (window) {
        left = cam_x - kWideExtra;
        left = std::min(left, level_px - kWideW);
        left = std::max(left, 0);
    }
    wide_frame_x = window ? cam_x - left : (kWideW - kFrameW) / 2;
    wide_left = window ? left : 0;

    // The game frame itself, at full width (the 4:3 overscan crop is a presentation choice
    // and is not applied here).
    for (int y = 0; y < kRows; y++) {
        std::memcpy(&wide_fb[static_cast<size_t>(y) * kWideW + wide_frame_x],
                    &framebuf[static_cast<size_t>(y) * kFrameW], kFrameW * sizeof(uint32_t));
    }
    if (!window || !hud_layer_enabled) return;

    // Row model. In a level the frame's split puts the status bar along the bottom
    // (hud_top): the rows above it carry the playfield's own scroll, and the status-bar rows
    // continue the playfield downward, so the ground runs on beside the bar instead of
    // sampling the bar's nametable (which holds the HUD, not level content). Level content
    // in those rows is drawn with the playfield's palette and banking, taken from the last
    // playfield row; out-of-level rows fall back to the row's backdrop.
    const int bar_top = hud_top;
    const int play_rows = bar_top < kRows ? bar_top : kRows;
    const int base_row = play_rows > 0 ? play_rows - 1 : 0;
    const int base_world_y = world_y_of(hud_lines[base_row].v) - base_row;
    auto row_state = [&](int y) -> const HudLine& {
        return y < play_rows ? hud_lines[y] : hud_lines[base_row];
    };
    auto row_world_y = [&](int y) -> int {
        return y < play_rows ? world_y_of(hud_lines[y].v) : base_world_y + y;
    };
    auto color_of = [&](const HudLine& h, unsigned index) -> uint32_t {
        // The row's own captured palette, not the live one: by the time this pass runs, palette
        // RAM already holds the status bar's colours (the game rewrites it during the frame).
        const uint8_t mask = (h.mask & 0x01) ? 0x30 : 0x3F;
        return nes_rgba(h.palram[Machine::pal_index(static_cast<uint16_t>(0x3F00 + index))] & mask);
    };

    // Level layout tables: TileLayout_ByTileset[tileset] points into the bank
    // TileLayoutPage_ByTileset[tileset] maps, and the runtime address's low 13 bits are
    // its offset within that 8 KB bank file.
    const unsigned layout_bank = prg[smb3::TileLayoutPage_ByTileset_ROM + tileset];
    const unsigned layout_base = prg[smb3::TileLayout_ByTileset_ROM + 2 * tileset] |
                                 (prg[smb3::TileLayout_ByTileset_ROM + 2 * tileset + 1] << 8);
    const uint8_t* tile_mem = sram;  // Tile_Mem is at $6000, the start of the save RAM

    // Background pattern of one extra pixel at a world position, plus the block's palette.
    auto bg_pattern = [&](int world_x, int world_y, const HudLine& h, unsigned& pal2) -> unsigned {
        pal2 = 0;
        if (world_x < 0 || world_x >= level_px || world_y < 0 || world_y >= kLevelHeightPx) return 0;
        const unsigned screen = static_cast<unsigned>(world_x) >> 8;
        const unsigned row = static_cast<unsigned>(world_y) >> 4;
        const uint8_t block = tile_mem[screen * kScreenBytes + row * 16 + ((world_x >> 4) & 15)];
        pal2 = (block >> 6) & 3;
        // Pages are top-left, bottom-left, top-right, bottom-right.
        const unsigned page =
            ((static_cast<unsigned>(world_x) >> 3) & 1) * 2 + ((static_cast<unsigned>(world_y) >> 3) & 1);
        const unsigned off = (layout_base & 0x1FFF) + page * 256 + block;
        if (layout_bank >= 32 || off >= 0x2000) return 0;  // table would run past the bank file
        const uint8_t tile = prg[layout_bank * 0x2000 + off];
        const unsigned bg_base = (h.ctrl & 0x10) ? 0x1000u : 0x0000u;
        const unsigned addr = bg_base + 16u * tile + (static_cast<unsigned>(world_y) & 7);
        const unsigned bank = h.chr_slot[(addr >> 10) & 7];
        const unsigned lo = chr[bank * 0x400 + (addr & 0x3FF)];
        const unsigned hi = chr[bank * 0x400 + ((addr + 8) & 0x3FF)];
        const unsigned bit = 7u - (static_cast<unsigned>(world_x) & 7);
        return (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1);
    };

    // Background columns outside the frame.
    for (int y = 0; y < kRows; y++) {
        const HudLine& h = row_state(y);
        const int world_y = row_world_y(y);
        const bool bg_on = h.on && (h.mask & 0x08) != 0;
        uint32_t* line = &wide_fb[static_cast<size_t>(y) * kWideW];
        for (int x = 0; x < kWideW; x++) {
            if (x >= wide_frame_x && x < wide_frame_x + kFrameW) continue;  // the frame stands
            unsigned pal2 = 0;
            const unsigned pat = bg_on ? bg_pattern(wide_left + x, world_y, h, pal2) : 0u;
            line[x] = color_of(h, pat ? pal2 * 4 + pat : 0);
        }
    }

    // Collect the frame's sprite pieces, then draw the parts that fall outside it.
    const int vscroll = mem16(*this, smb3::Level_VertScroll, smb3::Level_VertScrollH);
    struct Piece {
        int y0;         // OAM Y byte lifted to the object's page; shows at rows y0+1 .. y0+16
        int x;          // left column
        uint8_t pattern, attr;
        unsigned slot;  // OAM slot, for priority
    };
    Piece pieces[kMaxPieces];
    int np = 0;
    auto place_region = [&](unsigned base, int nominal_x, int nominal_y) {
        for (int k = 0; k < 6; k++) {
            const unsigned slot = base + 4u * static_cast<unsigned>(k);
            if (slot + 3 > 255) return;
            const uint8_t sy = oam[slot];
            if (sy >= 239) continue;  // vertically hidden in the frame (includes $F8 blanks)
            const int dx = static_cast<int8_t>(oam[slot + 3] - static_cast<uint8_t>(nominal_x));
            const int dy = static_cast<int8_t>(sy - static_cast<uint8_t>(nominal_y));
            // The drawers write pieces within the object's own box (up to 48 wide, 64 tall);
            // anything farther belongs to some other writer of this rotating region.
            if (dx < -48 || dx > 48 || dy < -56 || dy > 56) continue;
            if (np < kMaxPieces) pieces[np++] = Piece{nominal_y + dy, nominal_x + dx, oam[slot + 1], oam[slot + 2], slot};
        }
    };
    for (int s = 0; s < 8; s++) {
        if (ram[(smb3::Objects_State + s) & 0x7FF] == 0) continue;  // dead or empty slot
        place_region(ram[(smb3::Object_SprRAM + s) & 0x7FF] & 0xFC,
                     mem16(*this, smb3::Objects_X + s, smb3::Objects_XHi + s) - cam_x,
                     mem16(*this, smb3::Objects_Y + s, smb3::Objects_YHi + s) - vscroll);
    }
    // The player's six sprites sit at Player_SprOff ($28), the frame's own slot order.
    place_region(ram[smb3::Player_SprOff & 0x7FF] & 0xFC,
                 mem16(*this, smb3::Player_X, smb3::Player_XHi) - cam_x,
                 mem16(*this, smb3::Player_Y, smb3::Player_YHi) - vscroll);

    // Highest slot first, so lower (higher-priority) slots win where pieces overlap. The
    // player's slots are below every object's, so its extra copies stay on top too.
    std::sort(pieces, pieces + np, [](const Piece& a, const Piece& b) { return a.slot > b.slot; });
    for (int i = 0; i < np; i++) {
        const Piece& p = pieces[i];
        const unsigned spr_base = (p.pattern & 1) ? 0x1000u : 0x0000u;
        const bool hflip = (p.attr & 0x40) != 0;
        const bool vflip = (p.attr & 0x80) != 0;
        const bool behind = (p.attr & 0x20) != 0;
        const unsigned pal_base = 0x10u + (p.attr & 3) * 4;
        for (int j = 0; j < 16; j++) {
            const int y = p.y0 + 1 + j;  // the PPU shows a sprite one row below its Y byte
            if (y < 0 || y >= kRows) continue;
            const HudLine& h = row_state(y);
            if (!h.on) continue;                  // rendering off: the per-dot pass drew the row's colour
            if ((h.mask & 0x10) == 0) continue;   // sprites disabled on this row
            const unsigned diff = vflip ? 15u - static_cast<unsigned>(j) : static_cast<unsigned>(j);
            const unsigned addr =
                spr_base + 16u * (p.pattern & 0xFE) + ((diff & 8) << 1) + (diff & 7);
            const unsigned bank = h.chr_slot[(addr >> 10) & 7];
            uint8_t lo = chr[bank * 0x400 + (addr & 0x3FF)];
            uint8_t hi = chr[bank * 0x400 + ((addr + 8) & 0x3FF)];
            if (hflip) {
                lo = reverse_bits(lo);
                hi = reverse_bits(hi);
            }
            if (lo == 0 && hi == 0) continue;
            uint32_t* line = &wide_fb[static_cast<size_t>(y) * kWideW];
            const int world_y = row_world_y(y);
            for (int c = 0; c < 8; c++) {
                const int x = p.x + c + wide_frame_x;
                if (x < 0 || x >= kWideW) continue;
                if (x >= wide_frame_x && x < wide_frame_x + kFrameW) continue;  // the frame stands
                const unsigned pat = (((hi >> (7 - c)) & 1) << 1) | ((lo >> (7 - c)) & 1);
                if (pat == 0) continue;
                if (behind) {
                    unsigned pal2 = 0;  // background priority: an opaque BG pixel covers it
                    if (bg_pattern(wide_left + x, world_y, h, pal2)) continue;
                }
                line[x] = color_of(h, pal_base + pat);
            }
        }
    }
}
