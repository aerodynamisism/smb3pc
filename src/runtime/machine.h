// Runtime for the translated SMB3 game code.
//
// The 6502 core state lives here, together with the NES memory map, the MMC3
// mapper, the PPU register file and the frame timing. Generated bank functions
// (build/gen/bank_NN.cpp) operate on a Machine through the helpers below.
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include "runtime/apu.h"

struct Machine;
using BankFn = uint16_t (*)(Machine&, uint16_t);

enum class Mirror : uint8_t { Vertical, Horizontal };

struct Machine {
    // ---- 6502 registers ----
    uint8_t a = 0, x = 0, y = 0, s = 0xFD, p = 0x24;
    uint16_t pc = 0;
    int64_t cyc = 0;   // CPU cycles since power-on
    int64_t evt = 0;   // next cycle at which the dispatcher must regain control

    // ---- memory ----
    uint8_t ram[0x800] = {};
    uint8_t sram[0x2000] = {};
    const uint8_t* prg = nullptr;  // 256 KB PRG ROM (from the asset pack)
    const uint8_t* chr = nullptr;  // 128 KB CHR ROM (from the asset pack)
    uint8_t prg_slot[4] = {0, 1, 30, 31};  // 8 KB PRG bank per $8000/$A000/$C000/$E000
    uint8_t chr_slot[8] = {0, 1, 2, 3, 4, 5, 6, 7};  // 1 KB CHR bank per 1 KB window

    // ---- MMC3 mapper ----
    uint8_t mmc3_sel = 0;
    uint8_t prg_mode = 0;
    uint8_t chr_inv = 0;
    uint8_t mmc3_r[8] = {};
    Mirror mirror = Mirror::Vertical;
    uint8_t irq_latch = 0;
    uint8_t irq_counter = 0;
    bool irq_enable = false;
    bool irq_reload = false;
    bool irq_line = false;

    // ---- PPU state ----
    uint8_t vram[0x800] = {};   // two physical nametables
    uint8_t palram[32] = {};
    uint8_t oam[256] = {};
    uint8_t ppuctrl = 0, ppumask = 0, oamaddr = 0;
    uint16_t v = 0, t = 0;      // VRAM address / temporary address (loopy registers)
    uint8_t fine_x = 0;
    bool w = false;             // write toggle shared by $2005/$2006
    uint8_t read_buf = 0;       // $2007 read buffer
    bool vblank = false;
    bool sprite0_hit = false;
    bool nmi_pending = false;
    bool frame_done = false;
    bool sprite_overflow = false;

    // PPU pipeline state (dot level, as in nesalizer's ppu.cpp)
    unsigned pending_v_update = 0;       // dots until t is copied to v after a $2006 write
    uint8_t sec_oam[32] = {};
    uint8_t sec_oam_addr = 0;
    uint8_t oam_data = 0;
    uint8_t copy_sprite_signal = 0;
    bool oam_addr_overflow = false, sec_oam_addr_overflow = false, overflow_detection = false;
    bool s0_next = false, s0_cur = false;
    uint8_t nt_byte = 0, at_byte = 0, bg_byte_l = 0, bg_byte_h = 0;
    uint16_t bg_shift_l = 0, bg_shift_h = 0;
    unsigned at_shift_l = 0, at_shift_h = 0, at_latch_l = 0, at_latch_h = 0;
    uint8_t sprite_attribs[8] = {}, sprite_x[8] = {}, sprite_pat_l[8] = {}, sprite_pat_h[8] = {};
    uint8_t sprite_y = 0, sprite_index = 0;
    bool sprite_in_range = false;
    uint16_t ppu_addr_bus = 0;
    int64_t a12_low_dot = -1;            // dot at which PPU address line A12 last went low (-1: high)

    // ---- timing (NTSC: 262 lines of 341 PPU dots; CPU = PPU / 3) ----
    int64_t ppu_dot_pos = 0;    // PPU dots run so far (the PPU is synced to cyc * 3 before each register access)
    int ppu_line = 0;           // scanline of the next dot to run (0-261)
    int ppu_dot = 0;            // dot of the next dot to run (0-340)

    // ---- output ----
    uint32_t framebuf[256 * 240] = {};  // RGBA8: R in the low byte

    // ---- status-bar layer (see hud.cpp) ----
    // The bar is a display layer, not a second walk over the playfield's scroll. The split IRQ
    // marks its first row; the bar is then drawn once into its own 256-wide surface, in bar-local
    // coordinates, and that bitmap is composited over the frame. The surface is rebuilt only when
    // the state it was built from changes, so a steady frame is one state compare and one copy.
    static constexpr int kNoHud = 240;
    int hud_top = kNoHud;               // first status-bar row of this frame (last split IRQ wins)
    bool hud_layer_enabled = true;      // false draws every row from the per-dot pipeline
    // State each visible line is drawn from, captured at its dot 1. The game's split writes land
    // over several status-bar lines, so the capture keeps one row's worth per line.
    struct HudLine {
        uint16_t v = 0;
        uint8_t fine_x = 0, ctrl = 0, mask = 0;
        bool on = false;                // rendering was enabled
        uint8_t palram[32] = {};
        uint8_t chr_slot[8] = {};
        uint8_t oam[256] = {};          // sprite RAM: sprites are absolute-positioned, but the
                                        // per-dot pass mixes them with the (then wrong) playfield
                                        // background, so the bar layer re-evaluates them here
    };
    HudLine hud_lines[240] = {};
    struct HudLayer {
        // The bar picture: one RGBA row per bar scanline, 256 pixels wide, bar-local (row 0 is
        // the bar's first screen row).
        std::vector<uint32_t> pixels;
        std::vector<uint8_t> on;        // per row: the capture had rendering on (a real bar row)
        int rows = 0;
        // Everything the picture was built from. An equal source means an equal picture: that
        // compare is what lets a steady frame skip the redraw entirely. The anchor row supplies
        // the bar's single horizontal origin; each row keeps its own captured scroll, palette
        // and banking, because the split's staged writes and bank switches land row by row.
        struct Row {
            uint16_t v = 0;
            uint8_t fine_x = 0, ctrl = 0, mask = 0;
            uint8_t palram[32] = {};
            uint8_t chr_slot[8] = {};
            uint8_t on = 0;
        };
        struct Source {
            int top = -1;               // the split row (hud_top)
            int first = -1;             // first row the bar owns; above it is level
            int settled = -1;           // anchor row the bar's origin comes from
            uint16_t v = 0;
            uint8_t fine_x = 0;
            uint8_t oam[256] = {};
            uint8_t vram[0x800] = {};
            Mirror mirror = Mirror::Vertical;
            std::vector<Row> rows;
        } src;
    };
    HudLayer hud;
    void hud_capture(int line);
    void hud_compose();
    int hud_bar_first() const;          // first row after the level's last, -1 if none
    int hud_settled_row() const;        // first chain-continuous bar row, -1 if none
    bool hud_matches(int settled, int first) const;
    void hud_snapshot(int settled, int first);
    void hud_build();
    void hud_blit();
    // Camera the frame was drawn with, captured from RAM on the first rendered line. Horz_Scroll
    // is advanced in the NMI, after this frame was rendered, so reading it when the wide pass
    // runs can be a pixel or a whole tile ahead of the picture.
    uint16_t frame_scroll = 0;
    bool frame_scroll_set = false;

    // ---- widescreen output ----
    // The wide picture is the 4:3 frame with real level content added left and right.
    // Nothing here touches the game: wide_compose() runs after run_frame() and only
    // reads RAM, the captured rows and the ROM tables, so 4:3 output stays identical.
    //
    // Sprite Y records: the drawers hide a piece that lies outside the 256-pixel frame by
    // skipping its Y-byte write, so the staging buffer alone cannot say where the game put a
    // piece a wider frame would show. Generated code reports every Y value a drawer computes
    // (see translate.py) - the stores that happen and the ones a culling branch throws away -
    // and the OAM DMA snapshots the set into spr_y_frame, so the wide pass reads exactly the
    // pieces belonging to the frame `oam` holds, with no freshness guessing. 0xFE marks a
    // slot no drawer computed for that frame. Read-only side channel; never feeds the game.
    static constexpr uint8_t kSprYNone = 0xFE;
    uint8_t spr_y_rec[64] = {};        // Y values computed since the last OAM DMA
    uint64_t spr_y_seen = 0;           // which slots those are
    uint8_t spr_y_frame[64] = {};      // the set snapshotted with the frame in `oam`
    // The sprite owner each region belongs to, snapshotted with the same frame (the object
    // table's region assignments and positions are a frame ahead of the OAM copy once the
    // wide pass runs, so it needs the pair as the drawers saw them). Index 0-7: objects;
    // 8: the player.
    struct SprEntity {
        uint8_t on;
        uint8_t base;   // Object_SprRAM / Player_SprOff value, low bits cleared
        uint16_t x, y;  // world position
    };
    SprEntity spr_ents[9] = {};
    inline void spr_y_record(uint16_t ea, uint8_t val) {
        const unsigned slot = static_cast<unsigned>(ea - 0x0200) >> 2;
        if (slot < 64) {
            spr_y_rec[slot] = val;
            spr_y_seen |= 1ull << slot;
        }
    }
    inline void spr_y_store(uint16_t ea, uint8_t val) {
        spr_y_record(ea, val);
        wr(ea, val);
    }
    void oam_dma_snapshot();

    // Widescreen widens the game's object window so the added columns hold real, live objects
    // instead of empty ground. The game builds an object when the camera's look-ahead column
    // (Horz_Scroll + 272) reaches it and deletes it once it is 288-384 pixels ahead; the wide
    // picture reaches 341 pixels to the right of the camera, i.e. 85 past the frame's edge. The
    // spawner works in 16-pixel columns, so the window grows by kWideObjectAhead = 96, the
    // smallest step that covers those 85 columns: an object is created 368 pixels ahead of the
    // camera, 27 outside the picture, and walks in. These hooks are called from the two
    // translated instructions that add the look-ahead and delete table values (see
    // translate.py's WIDE_TUNE); in 4:3 they return their input, so the game is bit-for-bit the
    // same and only the wide picture changes.
    static constexpr int kWideObjectAhead = 96;
    int wide_ahead_col = -1;      // rightward look-ahead column the widened window reached
    int wide_behind_col = -1;     // leftward one (the game's look-behind), tracked the same way
    // Frames since the level started loading. The level loader builds the level and then sweeps the
    // camera over its first screen while calling the spawner once per 16-pixel column, which is
    // what creates the level's opening objects; that sweep uses the game's own look-ahead, so both
    // windows present it for those frames. It all happens under the level's start card.
    static constexpr int64_t kFrameCycles = 29780;
    int64_t wide_level_cyc = 0;
    // True until the rightward window's first column after a level load. Its opening column is the
    // game's own, and the window then walks out to the widened column a column per call, so the
    // objects between the two (the picture's right edge at the start of a level) are created
    // rather than skipped - the spawner only ever creates an object on the column it is shown.
    bool wide_window_from_level = true;
    // True while that walk-out is still going: those columns are inside the picture, and clipping
    // them would skip the objects sitting on them.
    bool wide_window_catching_up = false;
    // Called from the first instruction of Level_Initialize's init body (translate.py), i.e. once
    // per genuine level load. The window restarts there and nowhere else: the old test ("nothing
    // in Level_ObjectsSpawned is set") also fires when the last live object of a level is deleted
    // mid-level, and restarting then puts the next columns back inside the picture.
    void wide_level_load() {
        wide_ahead_col = wide_behind_col = -1;
        wide_window_from_level = true;
        wide_window_catching_up = false;
        wide_level_cyc = cyc;
    }

    // The spawner walks its look-ahead one 16-pixel column at a time and creates the objects that
    // sit exactly on that column (Level_ObjectsSpawnByScroll compares for equality and skips the
    // rest), so the window must never step over a column: an object skipped there is never created
    // again, which is what makes an enemy vanish from a level for good. Starting 96 pixels further
    // ahead would skip every object in the 96 pixels the picture just gained (that is how the
    // first pipe's plant disappeared), so the window walks out to the widened column one column per
    // call. It also never presents a column the picture already shows - an object created there is
    // seen to pop in - except while the level is being built, and during the rightward walk-out at
    // a level's start; both happen under the level's start card.
    inline uint8_t wide_spawn_lookahead(uint8_t v) {
        // Y is Scroll_LastDir here: 0 = scrolling right (+272), 1 = the -32 look-behind.
        if (!wide_enabled) return v;
        // Horz_Scroll ($FD) and Horz_Scroll_Hi ($12); the header cannot include the symbol table.
        const int cam = ram[0xFD] | (ram[0x12] << 8);
        const int dir = y & 1;
        const int base = dir ? -32 : 272;                   // LOSBS_LookAhead, sign extended
        // The game's own column and the widened one, both on its 16-pixel grid.
        const int game_col = (cam + base) & 0xFFF0;
        const int wide_col = (cam + base + (dir ? -kWideObjectAhead : kWideObjectAhead)) & 0xFFF0;
        // The first column past the picture on either side: a column nearer than this creates an
        // object where the picture already shows the tiles, so it is seen to pop in.
        const int pic_right = (cam + 256 + kWideExtra + 1) & 0xFFF0;
        const int pic_left = (cam - kWideExtra) & 0xFFF0;
        // The level loader builds the level and then sweeps the camera over its first screen
        // (calling the spawner once per 16-pixel column); that sweep is what creates the level's
        // opening objects, and it stays on the game's own column. It runs under the start card.
        const bool building = cyc - wide_level_cyc < 4 * kFrameCycles;
        int& tracked = dir ? wide_behind_col : wide_ahead_col;
        if (building) {
            tracked = game_col;
        } else if (dir == 0) {
            if (wide_window_from_level) {
                // First rightward column after the level was built: walk out from the column the
                // build sweep stopped on (or from the game's own, if it never ran) so the columns
                // up to the widened one are all presented.
                wide_window_from_level = false;
                wide_window_catching_up = true;
                if (tracked < 0) tracked = game_col;
            }
            if (tracked < wide_col) {
                if (wide_window_catching_up) {
                    // Walking out from the game's column at a level's start: these columns are
                    // inside the picture, which is fine there, and every one of them carries an
                    // object the level needs (the spawner never goes back to them).
                    tracked = tracked + 16 < wide_col ? tracked + 16 : wide_col;
                    if (tracked == wide_col) wide_window_catching_up = false;
                } else {
                    // The camera outran the window (a pipe, a camera snap): the columns in
                    // between are inside the picture now, so the window jumps out instead of
                    // creating objects that would be seen to pop in.
                    tracked = wide_col;
                }
            }
            if (tracked > wide_col) tracked = wide_col;   // camera moved back: already covered
            // Outside the level's build and its walk-out the window is never inside the picture.
            if (!wide_window_catching_up && tracked < pic_right) tracked = pic_right;
        } else {
            // The look-behind window just follows the camera: each column is presented as it
            // becomes the widened distance behind, which is well outside the picture, so there is
            // nothing to walk out to and nothing to clip.
            tracked = wide_col < pic_left ? wide_col : pic_left;
        }
        return static_cast<uint8_t>((tracked - cam) & 0xFF);
    }
    // The frame's sprite staging buffer only holds the pieces the game's drawers chose to draw
    // on a 256-pixel screen. A piece outside the screen is hidden by skipping its Y-byte store,
    // and some drawers then read that missing Y back (a troopa's head is drawn only if the body
    // under it was), so the frame alone cannot say where those pieces were. wide_after_state_action
    // runs once per object, from the object loop's own call site: an object that reaches beyond
    // the picture is then drawn a second time with its visibility bits cleared, so every piece
    // comes back with the pattern, attributes (and so palette) and low X byte the game's own
    // drawer gives it. The whole machine is rolled back around that run; only the pieces stay.
    struct WidePiece {
        int8_t dx, dy;   // piece position relative to the object, in pixels
        uint8_t tile, attr;
        // The OAM slot this piece would occupy in the game's own sprite rotation. It is signed:
        // a drawer may write before its region (a troopa's head, a piranha's pipe mask), and that
        // piece has to sort ahead of everything the object writes after it, so it must not be
        // folded to 0. Only the order matters, not the absolute value.
        int slot;
    };
    static constexpr int kWidePieces = 12;
    WidePiece wide_pieces[8][kWidePieces] = {};
    uint8_t wide_piece_count[8] = {};
    // True when wide_after_state_action actually re-ran this object's drawer for this frame: the
    // pieces (even none at all) then are the whole truth, and the wide pass never mixes them with
    // the frame's culled records.
    bool wide_piece_valid[8] = {};
    void wide_after_state_action();

    // Piranha plants skip their drawer on one frame in four (ObjNorm_Piranha: when Var4 & 3 is zero it
    // jumps past the draw), so the game's own frame has no sprites for them on that frame. The pieces a
    // plant drew on its last drawn frame are kept (offsets relative to the plant's screen position, the
    // same form as wide_pieces) and written back into the staging buffer on the skipped frame, before
    // the OAM DMA, so both the 4:3 frame and the widescreen pass show it. A plant holds for at most one
    // frame at a time: a skipped frame after a held one is shown without the plant, as the game does.
    WidePiece held_pieces[8][kWidePieces] = {};
    uint8_t held_count[8] = {};
    uint8_t held_id[8] = {};
    bool held_valid[8] = {};
    void sprite_hold_before_dma();
    // True on a frame where the plant's drawer skips its draw (ObjNorm_Piranha: Var4 + 2 when vertically
    // flipped, & 3 == 0).
    bool plant_skip_phase(int s) const;
    bool sandbox = false;  // true while a drawer is re-run: PPU and APU writes are dropped

    // How far past the spawn window an object may drift before the game's own off-screen delete
    // removes it (which also re-arms it to be created again). The game's nearest right-hand
    // distance is 288, and an object created at the widened window sits 368 pixels ahead, so
    // with the window widened by 96 alone the two would be sixteen pixels apart: an object that
    // walks away from the camera - a red koopa on a ledge, a bullet bill - would be created and
    // removed again and again at the picture's edge, never getting anywhere. The slack puts a
    // hundred pixels between them, which is room enough for the object to move into the picture
    // (or for the camera to catch up) before the game may drop it.
    static constexpr int kWideObjectSlack = 176;
    // The carry (right) or borrow (left) out of the low byte. The game adds the table's high byte
    // separately (wide_delete_ahead_hi), so the slack's carry has to be carried over by hand:
    // without it, a distance whose low byte wraps lands 256 pixels too close. That is the piranha
    // plant's row (Object_DeleteOffScreen_N2): 384 right and -128 left became 304 and -48, so a
    // plant still inside the 16:9 picture was removed. Set by the low-byte hook, used by the high one.
    int wide_delete_fix = 0;
    inline uint8_t wide_delete_ahead(uint8_t v) {
        // Even entries of the delete table are the right-hand distances (high byte $01), odd ones
        // the left-hand ones ($FF). The wide picture reaches kWideObjectAhead past the frame on
        // both sides, so both distances grow, and an object is only removed once it is outside
        // the 16:9 picture instead of popping while it is still on screen.
        if (!wide_enabled) {
            wide_delete_fix = 0;
            return v;
        }
        const int sum = (y & 1) ? v - kWideObjectSlack : v + kWideObjectSlack;
        wide_delete_fix = sum >= 256 ? 1 : sum < 0 ? -1 : 0;
        return static_cast<uint8_t>(sum);
    }
    // The table's high byte ($01 right, $FF left), with the carry or borrow from wide_delete_ahead.
    inline uint8_t wide_delete_ahead_hi(uint8_t v) {
        const int fix = wide_delete_fix;
        wide_delete_fix = 0;
        return static_cast<uint8_t>(v + fix);
    }
    static constexpr int kWideW = 426;    // 256 + 2 * 85: 16:9 at NES pixel scale
    static constexpr int kWideExtra = 85;
    // The picture's middle is the frame with these columns cropped on each side; kWideBox is the
    // widest object's sprite box. Both the re-run and the wide pass test an object against the
    // same two numbers, so a drawer is never needed for a frame that skipped it.
    //
    // Two columns deeper than the NES's own 8-pixel blanking: while the picture scrolls, the
    // frame's last columns can show a stale tile fetch - a sliver of ground texture drawn over
    // the cloud band for a frame or two, which the TV's overscan hides on real hardware. Every
    // sliver seen in a full sweep of a scrolling level stays within those two columns; the
    // margin pass draws them from the level model instead.
    static constexpr int kWideCrop = 12;
    static constexpr int kWideBox = 48;
    bool wide_enabled = false;            // user setting; see set_widescreen()
    std::vector<uint32_t> wide_fb;        // kWideW * 240 RGBA, R in the low byte
    // kWideW * 240: pixels of the added columns a piece has claimed. The hardware picks one
    // sprite per pixel - the first in OAM order with an opaque bit there - and a piece it picks
    // owns the pixel even when the piece's behind bit then gives the pixel to the background;
    // see the sprite-priority note in wide.cpp's paint.
    std::vector<uint8_t> wide_cover;
    int wide_frame_x = kWideExtra;        // x of the game frame's left column in wide_fb
    int wide_left = 0;                    // world X at wide_fb column 0 (in-level frames)
    void set_widescreen(bool on);
    void wide_compose();                  // call right after run_frame() while wide_enabled

    // ---- input ----
    uint8_t joy[2] = {0, 0};        // A B Select Start Up Down Left Right (bit 0 = A)
    uint8_t joy_shift[2] = {0, 0};
    bool joy_strobe = false;

    // ---- sound ----
    // Music and sound effects each drive their own 2A03 voice set, so an effect never silences
    // or retunes a music channel. Generated code sets snd_owner at the sound routines (see
    // translate.py, SND_ROUTINE_OWNER) and every APU write is routed by it.
    enum : uint8_t { kSndMusic = 0, kSndSfx = 1, kSndBoth = 2 };
    uint8_t snd_owner = kSndMusic;
    uint8_t snd_sav[256] = {};   // owner saved at each JSR, restored at the matching RTS (indexed by S)
    Apu apu;                     // music voices; $4015 status reads come from here
    Apu sfx_apu;                 // sound-effect voices
    void render_audio(int64_t until_cyc, std::vector<int16_t>& out);  // both voice sets, summed
    void set_audio_rate_scale(double scale);
    // Level loads re-initialise only the music voice set (the game's own code drives
    // Sound_PlayMusic on the music owner), so channel state left behind by the previous
    // level used to keep sounding under the new one. Called by generated code at the
    // first instruction of Level_Initialize's init body (see translate.py); nothing to
    // route here, it is a sink reset.
    void reset_sfx_apu();

    // ---- lifecycle ----
    void reset();
    void run_frame();          // runs until the next vblank has started and the frame is ready

    // ---- 6502 bus ----
    inline uint8_t rd(uint16_t addr) {
        if (addr < 0x2000) return ram[addr & 0x7FF];
        if (addr >= 0x8000) return prg[static_cast<size_t>(prg_slot[(addr >> 13) & 3]) * 0x2000 + (addr & 0x1FFF)];
        return rd_io(addr);
    }
    inline void wr(uint16_t addr, uint8_t val) {
        if (addr < 0x2000) { ram[addr & 0x7FF] = val; return; }
        if (addr >= 0x8000) { wr_mapper(addr, val); return; }
        wr_io(addr, val);
    }
    inline uint16_t rd16(uint16_t addr) { return static_cast<uint16_t>(rd(addr) | (rd(addr + 1) << 8)); }

    uint8_t rd_io(uint16_t addr);
    void wr_io(uint16_t addr, uint8_t val);
    void wr_mapper(uint16_t addr, uint8_t val);

    // ---- stack ----
    inline void push(uint8_t val) { ram[0x100 | s] = val; s--; }
    inline uint8_t pull() { s++; return ram[0x100 | s]; }
    inline void push16(uint16_t val) { push(static_cast<uint8_t>(val >> 8)); push(static_cast<uint8_t>(val & 0xFF)); }
    inline uint16_t pull16() { uint8_t lo = pull(); uint8_t hi = pull(); return static_cast<uint16_t>(lo | (hi << 8)); }

    // ---- addressing helpers (page-cross penalties added when pen is true) ----
    inline uint16_t zpx(uint8_t zp) const { return static_cast<uint8_t>(zp + x); }
    inline uint16_t zpy(uint8_t zp) const { return static_cast<uint8_t>(zp + y); }
    inline uint16_t eaAbsX(uint16_t base, bool pen) {
        uint16_t ea = static_cast<uint16_t>(base + x);
        if (pen && ((base ^ ea) & 0xFF00)) cyc += 1;
        return ea;
    }
    inline uint16_t eaAbsY(uint16_t base, bool pen) {
        uint16_t ea = static_cast<uint16_t>(base + y);
        if (pen && ((base ^ ea) & 0xFF00)) cyc += 1;
        return ea;
    }
    inline uint16_t eaIndX(uint8_t zp) {
        uint8_t ptr = static_cast<uint8_t>(zp + x);
        uint8_t lo = rd(ptr);
        uint8_t hi = rd(static_cast<uint8_t>(ptr + 1));
        return static_cast<uint16_t>(lo | (hi << 8));
    }
    inline uint16_t eaIndY(uint8_t zp, bool pen) {
        uint8_t lo = rd(zp);
        uint8_t hi = rd(static_cast<uint8_t>(zp + 1));
        uint16_t base = static_cast<uint16_t>(lo | (hi << 8));
        uint16_t ea = static_cast<uint16_t>(base + y);
        if (pen && ((base ^ ea) & 0xFF00)) cyc += 1;
        return ea;
    }
    inline uint16_t jmpInd(uint16_t ptr) {
        // NMOS 6502 page-wrap bug: the high byte is fetched within the same page
        uint8_t lo = rd(ptr);
        uint16_t hi_addr = static_cast<uint16_t>((ptr & 0xFF00) | ((ptr + 1) & 0x00FF));
        return static_cast<uint16_t>(lo | (rd(hi_addr) << 8));
    }

    // ---- flags and ALU ----
    inline void nz(uint8_t val) {
        p = static_cast<uint8_t>((p & ~0x82) | (val & 0x80) | (val == 0 ? 0x02 : 0));
    }
    inline uint8_t nzv(uint8_t val) { nz(val); return val; }
    inline void adc(uint8_t val) {
        uint16_t sum = static_cast<uint16_t>(a + val + (p & 1));
        uint8_t r = static_cast<uint8_t>(sum);
        p = static_cast<uint8_t>(p & ~0x41);
        if (sum > 0xFF) p |= 0x01;
        if ((~(a ^ val) & (a ^ r) & 0x80)) p |= 0x40;
        a = r;
        nz(a);
    }
    inline void sbc(uint8_t val) { adc(static_cast<uint8_t>(~val)); }
    inline void cmp(uint8_t reg, uint8_t val) {
        uint8_t r = static_cast<uint8_t>(reg - val);
        p = static_cast<uint8_t>(p & ~0x01);
        if (reg >= val) p |= 0x01;
        nz(r);
    }
    inline void bit(uint8_t val) {
        p = static_cast<uint8_t>(p & ~0xC2);
        p |= static_cast<uint8_t>(val & 0xC0);
        if ((a & val) == 0) p |= 0x02;
    }
    inline uint8_t asl(uint8_t val) {
        p = static_cast<uint8_t>((p & ~0x01) | (val >> 7));
        return nzv(static_cast<uint8_t>(val << 1));
    }
    inline uint8_t lsr(uint8_t val) {
        p = static_cast<uint8_t>((p & ~0x01) | (val & 1));
        return nzv(static_cast<uint8_t>(val >> 1));
    }
    inline uint8_t rol(uint8_t val) {
        uint8_t c = p & 1;
        p = static_cast<uint8_t>((p & ~0x01) | (val >> 7));
        return nzv(static_cast<uint8_t>((val << 1) | c));
    }
    inline uint8_t ror(uint8_t val) {
        uint8_t c = p & 1;
        p = static_cast<uint8_t>((p & ~0x01) | (val & 1));
        return nzv(static_cast<uint8_t>((val >> 1) | (c << 7)));
    }

    // ---- interrupts / errors ----
    void interrupt(uint16_t vector);
    uint16_t vectorIrq() { return rd16(0xFFFE); }
    // After CLI or PLP the I flag changes; an IRQ that is now enabled is taken after the next
    // instruction, so wake the dispatcher at once.
    void kick_irq() { if (irq_line && !(p & 0x04)) evt = cyc + 1; }
    void brk() { push16(static_cast<uint16_t>(pc + 2)); push(static_cast<uint8_t>(p | 0x30)); p |= 0x04; }
    [[noreturn]] uint16_t badEntry(uint16_t entry, int bank);

    // ---- internals used by the frame loop ----
    uint16_t dispatch(uint16_t addr);
    void process_events();
    void ppu_sync();                        // runs the PPU dots up to the CPU's current cycle
    void ppu_step();                        // runs one PPU dot and the line/frame events on it
    void mmc3_clock();
    void ppu_set_addr(uint16_t addr);       // drives the PPU address bus and watches A12 for MMC3
    void update_mmc3_banks();
    void render_dot(int line, int dot);     // renders one PPU dot (pixels, v/t, sprites)
    void ppu_data_bump(bool rendering_line);  // $2007 access side effect on v
    bool rendering_line_now() const;          // the scanline the CPU is currently in is rendered
    bool rendering_enabled() const { return (ppumask & 0x18) != 0; }

    // ---- PPU memory ----
    uint8_t chr_read(uint16_t addr) const { return chr[static_cast<size_t>(chr_slot[(addr >> 10) & 7]) * 0x400 + (addr & 0x3FF)]; }
    uint8_t ppu_mem_read(uint16_t addr);
    void ppu_mem_write(uint16_t addr, uint8_t val);
    size_t nt_index(uint16_t addr) const;
    static size_t pal_index(uint16_t addr) {
        size_t i = addr & 0x1F;
        if ((i & 0x13) == 0x10) i &= ~0x10u;
        return i;
    }
};
