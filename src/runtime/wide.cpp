// Widescreen (16:9) compose pass.
//
// The 256x240 frame the emulator renders stays byte-for-byte what the NES would show:
// this pass runs after run_frame(), at the same vblank moment as hud_compose(), and reads
// game state only (RAM, sprite RAM, Tile_Mem, the per-scanline PPU snapshots and the PRG
// ROM's level tables). It copies that frame into a 426x240 picture and fills the 85
// columns on either side with the level content that is really there. None of this runs
// while 4:3 is selected; in widescreen the one thing outside this pass is the object window,
// which the game itself is allowed to reach 96 pixels further ahead (see the last source note
// below and Machine::wide_spawn_lookahead).
//
// Model: the frame's left edge sits at world X S (the scroll the frame was drawn with) and
// the frame is 256 world pixels wide; the wide picture widens that window to 426. While the
// window fits inside the level the frame stays centred and the extra 85 columns show real
// level columns to either side. At either end of the level the window stops (the frame
// slides off-centre instead) so nothing outside the level is ever invented. Levels shorter
// than the wide window keep the frame centred on black, as do non-level screens (title, map,
// ending).
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
//     the same per-line snapshots the bar layer (hud.cpp) builds the status bar from, so extra columns
//     shaded or bank-switched by autoscroll effects match the visible ones. The rows the
//     frame gives to the status bar (hud_top..239) continue the playfield downward, drawn
//     with the last playfield row's palette and banking, so the level's ground runs on
//     beside the bar; the bar's own nametable holds the HUD, not level content.
//   - Sprites: the game's own pieces, at their true world position. Each drawer writes a
//     four-byte sprite (Y, pattern, attributes, X) per piece and hides a piece outside the
//     256-pixel frame by skipping its Y-byte store; the generated code reports every Y value a
//     drawer computes, stored or skipped (see translate.py), and the OAM DMA snapshots those
//     records with the frame. Lifting the 8-bit values onto the 256-pixel page of the object
//     (or the player) that computed them recovers each piece's real column, so the extra
//     columns never show a stale sprite from an earlier user of a shared region. The pieces then
//     decide pixels among themselves by OAM order, as the PPU's sprite chain does: the first
//     piece with an opaque bit at a pixel owns it - even when its behind bit then gives the
//     pixel to an opaque background pixel underneath, which is how a piranha plant's green pipe
//     mask keeps its body inside the pipe - and a piece it beat never shows through. A piece a
//     drawer never computes at all - because it reads back a Y byte the frame's culling left
//     behind - is recovered by drawing the object a second time (see the paragraph below).
//   - Enemies in the added columns: the game creates an object only when the camera's look-ahead
//     column (Horz_Scroll + 272) reaches it, i.e. everything that exists sits at the frame's own
//     right edge. Widescreen therefore widens that window by 96 pixels, the smallest 16-pixel
//     column step that covers the 85 added columns, and the deletion window by 176 on both sides
//     (Machine::wide_spawn_lookahead / wide_delete_ahead, wired up by translate.py's WIDE_TUNE).
//     The delete window moves further: the game's own left-hand distance (48-128 pixels behind
//     the camera) sits inside the added columns, and its nearest right-hand one (288) would leave
//     an object created at the spawn window only sixteen pixels from removal, so it could never
//     walk in. With both widened an enemy stays until it has left the wide picture, and a new one
//     has about a hundred pixels to move before the game may remove it. The spawner compares its
//     look-ahead column for equality and skips the rest, so the widened value climbs one 16-pixel
//     column per call (Machine::wide_spawn_lookahead): no column is ever stepped over. It also
//     never presents a column the picture already shows - an object created there is seen to pop
//     in. Two moments are exempt, both under a level's start card: the frames the level loader
//     spends building the level (its sweep needs the game's own column to create the level's
//     opening objects) and the rightward window's walk-out from that column to the widened one.
//     They are real objects: no sprite is invented for them, and once spawned nothing about them
//     is left to this pass. 4:3 turns the hooks off, so the game is unchanged there.
//
// An object that reaches beyond the picture is also drawn a second time, inside the frame's own
// object loop (Machine::wide_after_state_action): the frame hides a piece by skipping its Y-byte
// store, and a drawer that reads that byte back (a troopa's head is drawn only if the body under
// it was) would otherwise leave the piece out or place it at a stale row. The state action
// recomputes the visibility bits from the object's screen position, so the re-run is given the
// scrolls that put the object at 100,100 - nothing culled, every piece written at the pattern,
// attributes, palette corrections and position the drawer itself chooses - and the machine is
// rolled back around it, so the game sees nothing. It starts from the position in the OAM DMA
// snapshot, the one the frame's own draw used, so a piece a drawer places against the level
// rather than against itself (a piranha plant's pipe mask) comes back on the frame's row. Those
// pieces are then the whole truth for that frame, even when there are none (the object is
// hidden): mixing them with the frame's culled records would move a sprite by a pixel whenever
// the frame happened to cull a different piece.
//
// The picture's middle is the 4:3 frame with its overscan columns cropped (four columns further in
// than 4:3 mode's own crop, so the half-tile smear at the frame's edge never shows and neither does
// the stale tile fetch a scrolling frame can leave in its last two columns), pasted once
// and never redrawn; only the columns outside it are composed here, so the visible playfield is
// the game's own pixels. The two 12-column strips the crop removes are composed like any other
// extra column (background from Tile_Mem, sprites from the frame's records and the objects'
// second draw), so the crop leaves no hole, no duplicate and no stripe. The status bar is pasted
// at the centre of the picture, so the HUD stays centred even while the frame has slid toward a
// level boundary. The added columns are aligned
// to the scroll the frame was actually drawn with, taken from the first playfield row's captured
// v register rather than from Horz_Scroll (which the NMI has already advanced for the next
// frame); using the live value makes the extra columns slip a pixel or two against the frame
// while the camera moves.
//
// Known differences from a hypothetical 426-pixel NES running SMB3, kept deliberately:
//   - In widescreen an enemy is created, and starts moving, 96 pixels further ahead than the
//     4:3 game would create it, because the picture reaches further: that is the window
//     widening above, and it is the only gameplay difference widescreen mode has.
//   - The 8-sprites-per-line hardware limit and the sprite-0 hit apply to the frame the
//     game drew; the extra columns can show more sprites on a line than a real PPU would.
//   - A drawer that hides a piece some other way (skipping a run of stores that is not all
//     Y bytes, or writing a blank) leaves no usable record, and the player is not drawn a
//     second time, so such a piece is missing from the extra columns instead of being drawn in
//     the wrong place. Every standard enemy, item and player drawer goes through a plain
//     Y-byte store.
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
// Frame columns the wide picture crops (the re-run uses the same numbers, so an object the wide
// pass asks about has always been drawn: Machine::kWideCrop / kWideBox).
constexpr int kOverscan = Machine::kWideCrop;
constexpr int kObjectBox = Machine::kWideBox;
constexpr int kRows = 240;
constexpr int kScreens = 15;                     // Tile_Mem holds 15 screens
constexpr int kScreenBytes = 0x1B0;              // 16 columns x 27 rows of blocks
constexpr int kBlockRows = 27;
constexpr int kLevelHeightPx = kBlockRows * 16;  // 432

// Level tilesets whose blocks have the standard four-page layout table (the file header
// explains the format). 0 is the world map and 15+ are the bonus games, not levels.
constexpr int kFirstTileset = 1;
constexpr int kLastTileset = 14;                 // 14 = underground, same mechanism

// A drawer may write outside its own six-sprite region (a troopa's head sits before it), so an
// object can hand back a dozen pieces; eight objects plus the player's six records.
constexpr int kMaxPieces = 8 * Machine::kWidePieces + 6;

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
    // The window starts over whenever the mode comes on, but not from the game's own column: the
    // columns up to the widened one have already been presented (the 4:3 spawner reaches 272
    // pixels ahead, and every object out there exists), so the first call jumps straight to the
    // widened column instead of walking back through the picture. A level load clears the flag
    // again; see wide_level_load.
    wide_ahead_col = -1;
    wide_behind_col = -1;
    wide_window_from_level = false;
    wide_window_catching_up = false;
    // Pieces captured before the mode was switched are not this frame's; the next object loop
    // fills them again.
    std::memset(wide_piece_count, 0, sizeof wide_piece_count);
    std::memset(wide_piece_valid, 0, sizeof wide_piece_valid);
    if (on && wide_fb.size() != static_cast<size_t>(kWideW) * kRows) {
        wide_fb.assign(static_cast<size_t>(kWideW) * kRows, 0xFF000000u);
        wide_cover.assign(static_cast<size_t>(kWideW) * kRows, 0);
    }
}

void Machine::wide_compose() {
    if (!wide_enabled || wide_fb.size() != static_cast<size_t>(kWideW) * kRows) return;
    std::fill(wide_fb.begin(), wide_fb.end(), 0xFF000000u);
    if (wide_cover.size() != wide_fb.size()) wide_cover.assign(wide_fb.size(), 0);

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
    // The frame's left edge in world columns. Horz_Scroll is the camera the game will draw the
    // *next* frame with: it is updated in the NMI, after this frame was rendered, so taking it
    // live puts the added columns a pixel or a whole tile off the pasted frame whenever the
    // camera moved during the frame - which reads as the extra columns jittering against the
    // 4:3 picture. The PPU snapshots the value on the frame's first rendered line instead.
    int cam_x = mem16(*this, smb3::Horz_Scroll, smb3::Horz_Scroll_Hi);
    if (frame_scroll_set) {
        const int live_low = cam_x & 0xFF;
        const int low = frame_scroll & 0xFF;
        int frame_cam = (cam_x & ~0xFF) | low;
        if (low - live_low > 128) frame_cam -= kFrameW;
        else if (low - live_low < -128) frame_cam += kFrameW;
        cam_x = frame_cam;
    }

    // Place the wide window around the frame: centred while it fits, stopped at the level
    // ends (the frame slides toward the other edge). The camera only hugs the window while it
    // is inside the level; mid-transition (a junction reloading the level) Horz_Scroll and
    // Level_Width briefly describe different levels, and a camera outside this level would put
    // the frame outside the picture, so those frames keep the frame centred on black.
    bool window = level_frame && level_px >= kWideW && cam_x >= 0 && cam_x <= level_px - kFrameW;
    int left = 0;
    int frame_x = (kWideW - kFrameW) / 2;
    if (window) {
        left = cam_x - kWideExtra;
        left = std::min(left, level_px - kWideW);
        left = std::max(left, 0);
        frame_x = cam_x - left;  // 0..kWideExtra: the frame always fits inside the window
    }
    wide_frame_x = std::clamp(frame_x, 0, kWideW - kFrameW);
    wide_left = window ? cam_x - wide_frame_x : 0;

    // The game frame, with its overscan columns cropped: screen columns 0-11 and 244-255 are left
    // to the extra-column pass below. That is four columns further in than the NES's own 8-pixel
    // blanking on each side, so the half-tile smear at the frame's edge never shows, and neither
    // does the stale tile fetch a scrolling frame can leave in its last two columns (a sliver of
    // ground texture over the cloud band; the TV's overscan hides it on real hardware); the
    // playfield is pasted at wide_frame_x (the game's own frame position, which slides toward a
    // level boundary) and the status bar's rows are pasted at the centre of the picture, so the
    // HUD stays centred while that happens. The boundary is the bar layer's first row, not the
    // split IRQ: the level still owns the rows between the two, and those must stay aligned with
    // the playfield they belong to instead of being centred with the bar.
    const int bar_top = (hud_layer_enabled && hud.src.first > 0) ? hud.src.first : hud_top;
    const int frame_visible = kFrameW - 2 * kOverscan;
    const bool bar = hud_layer_enabled && bar_top < kRows;
    auto paste_x = [&](int y) -> int {
        if (bar && y >= bar_top) return (kWideW - frame_visible) / 2;
        return wide_frame_x + kOverscan;
    };
    for (int y = 0; y < kRows; y++) {
        std::memcpy(&wide_fb[static_cast<size_t>(y) * kWideW + paste_x(y)],
                    &framebuf[static_cast<size_t>(y) * kFrameW + kOverscan],
                    frame_visible * sizeof(uint32_t));
    }
    if (!window || !hud_layer_enabled) return;

    // Row model. In a level the frame's split puts the status bar along the bottom
    // (hud_top): the rows above it carry the playfield's own scroll, and the status-bar rows
    // draw with the playfield's palette and banking, taken from the last playfield row (the
    // bar's own nametable holds the HUD, not level content). The bar rows keep the picture's
    // black beside the centred bar (the fill below skips them), so the bar spans the full
    // width instead of level content running on next to the HUD; out-of-level rows fall back
    // to the row's backdrop.
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

    // Background columns outside the pasted frame. The bar rows are skipped: their margins are
    // the picture's own black, so the status bar reads as one band across the full width.
    for (int y = 0; y < kRows; y++) {
        if (bar && y >= bar_top) continue;
        const HudLine& h = row_state(y);
        const int world_y = row_world_y(y);
        const bool bg_on = h.on && (h.mask & 0x08) != 0;
        const int px = paste_x(y);
        uint32_t* line = &wide_fb[static_cast<size_t>(y) * kWideW];
        for (int x = 0; x < kWideW; x++) {
            if (x >= px && x < px + frame_visible) continue;  // the pasted frame stands
            unsigned pal2 = 0;
            const unsigned pat = bg_on ? bg_pattern(wide_left + x, world_y, h, pal2) : 0u;
            line[x] = color_of(h, pat ? pal2 * 4 + pat : 0);
        }
    }

    // Sprite pieces in the columns beside the pasted frame.
    //
    // The frame's staging buffer only holds the pieces the game drew on a 256-pixel screen, and a
    // piece outside it is hidden by skipping its Y-byte store. Some drawers then read that missing
    // Y back - a troopa's head is drawn only when the body under it was - so lifting the frame's
    // records alone leaves those pieces out or puts them at a stale row. An object that reaches
    // into the added columns is drawn again when the object loop reaches it, with its visibility
    // bits cleared (Machine::wide_after_state_action): every piece comes back with the pattern,
    // attributes (and so palette) and position the game's own drawer gives it, and the machine is
    // rolled back afterwards.
    //
    // The player is never re-run: its action moves it, and it is never more than a few columns
    // outside the frame, so it keeps the frame's Y records. The X byte a drawer wrote is relative
    // to the scroll it used and the Y byte to the vertical scroll, so lifting both onto the
    // entity's nominal on-screen position recovers the piece's real column and row.
    const int vscroll = mem16(*this, smb3::Level_VertScroll, smb3::Level_VertScrollH);
    struct Piece {
        int y0;         // piece's Y byte lifted to the object's page; shows at rows y0+1 .. y0+16
        int x;          // left column, in world columns relative to the frame
        uint8_t pattern, attr;
        int slot;       // OAM slot, for priority (may be negative, see Machine::WidePiece)
    };
    Piece pieces[kMaxPieces];
    int np = 0;
    auto add = [&](int nominal_x, int nominal_y, uint8_t x, uint8_t ry, uint8_t tile,
                   uint8_t attr, int slot) {
        // The bytes are screen-relative; lifting them onto the entity's nominal on-screen
        // position is what turns an X byte of $2A for something 298 pixels right of the frame
        // into world S+298. Anything farther than the widest drawer box from the entity is a
        // stale read from another writer of this rotating region, so it is left out.
        const int dx = static_cast<int8_t>(static_cast<uint8_t>(x - (nominal_x & 0xFF)));
        const int dy = static_cast<int8_t>(static_cast<uint8_t>(ry - (nominal_y & 0xFF)));
        if (dx < -64 || dx > 64 || dy < -64 || dy > 64) return;
        if (np < kMaxPieces) pieces[np++] = Piece{nominal_y + dy, nominal_x + dx, tile, attr, slot};
    };
    for (int s = 0; s < 9; s++) {
        const SprEntity& e = spr_ents[s];
        if (!e.on) continue;  // dead or empty slot
        const int nominal_x = static_cast<int>(e.x) - cam_x;
        const int nominal_y = static_cast<int>(e.y) - vscroll;
        // Wholly inside the pasted frame: the game's own pixels already show it. The box is the
        // widest object, so a piece straddling the frame's edge is never skipped.
        if (nominal_x >= kOverscan && nominal_x + kObjectBox <= kFrameW - kOverscan) continue;
        // Pieces the object's own drawer produced when the wide pass asked it to draw the object
        // with nothing culled (Machine::wide_after_state_action). They carry their position
        // relative to the object, already measured with the drawer's own camera. When the re-run
        // happened, its pieces are the whole truth - even none at all, which means the object is
        // hidden this frame - so they are never mixed with the frame's culled records: mixing the
        // two would move a sprite by a pixel whenever the frame happened to cull a different piece.
        if (s < 8 && wide_piece_valid[s]) {
            for (int i = 0; i < wide_piece_count[s]; i++) {
                const WidePiece& w = wide_pieces[s][i];
                if (np < kMaxPieces) {
                    pieces[np++] = Piece{nominal_y + w.dy, nominal_x + w.dx, w.tile, w.attr, w.slot};
                }
            }
            continue;
        }
        // An object whose drawer was not re-run this frame drew nothing this frame either (it was
        // spawned or removed after the object loop), so there are no pieces of it to draw. Its
        // region in the frame's sprite buffer still holds the previous owner's bytes, and drawing
        // those at the new object's position is a sprite that blinks on for a single frame.
        if (s < 8) continue;
        // The player keeps the frame's own records: it is never re-run (its action moves it), and
        // it is never more than a few columns outside the frame, so every piece of it that the
        // picture needs was written this frame and its Y byte recorded even when the narrow frame
        // culled the store.
        for (unsigned k = 0; k < 6; k++) {
            const int slot = static_cast<int>(e.base) + 4 * static_cast<int>(k);
            if (slot + 3 > 255) break;
            const uint8_t ry = spr_y_frame[slot >> 2];
            if (ry == kSprYNone || ry >= 239) continue;  // not computed, or below the picture
            add(nominal_x, nominal_y, oam[slot + 3], ry, oam[slot + 1], oam[slot + 2], slot);
        }
    }

    // Draw one piece's pixels into the added columns. The pasted frame always wins: a column the
    // 4:3 picture already shows is left exactly as the game drew it.
    auto paint = [&](const Piece& p) {
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
            uint8_t* cover = &wide_cover[static_cast<size_t>(y) * kWideW];
            const int world_y = row_world_y(y);
            const int px = paste_x(y);
            for (int c = 0; c < 8; c++) {
                const int x = p.x + c + wide_frame_x;
                if (x < 0 || x >= kWideW) continue;
                if (x >= px && x < px + frame_visible) continue;  // the pasted frame stands
                const unsigned pat = (((hi >> (7 - c)) & 1) << 1) | ((lo >> (7 - c)) & 1);
                if (pat == 0) continue;
                // The hardware holds a sprite-priority contest per pixel: the first piece in OAM
                // order with an opaque bit there wins, and a piece it beat never shows through -
                // not even where the winner's behind bit gives the pixel to the background. A
                // piranha plant hides inside its pipe exactly this way: a green mask piece sits
                // ahead of its body, and where the pipe's opaque pixels reject the mask the pipe
                // itself covers the body. Pieces are painted lowest OAM slot first, so a claimed
                // pixel belongs to a piece that was ahead of this one.
                if (cover[x]) continue;
                cover[x] = 1;
                if (behind) {
                    unsigned pal2 = 0;  // background priority: an opaque BG pixel covers it
                    if (bg_pattern(wide_left + x, world_y, h, pal2)) continue;
                }
                line[x] = color_of(h, pal_base + pat);
            }
        }
    };

    // Lowest OAM slot first: it has the sprite priority, claims its pixels and closes them to
    // the pieces behind it (which may still paint pixels the winner left transparent).
    std::sort(pieces, pieces + np, [](const Piece& a, const Piece& b) { return a.slot < b.slot; });
    std::fill(wide_cover.begin(), wide_cover.end(), 0);
    for (int i = 0; i < np; i++) paint(pieces[i]);
}
