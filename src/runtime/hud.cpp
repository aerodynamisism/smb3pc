// Status-bar (HUD) layer.
//
// The bar is a display layer of its own: it is not drawn through the playfield's scroll
// pipeline. The split IRQ marks the bar's first row (hud_top); the bar is then drawn once into a
// 256-wide surface in bar-local coordinates - the bar's own first visible column is bar pixel 0,
// so the layer has a single origin and no per-pixel scroll to reproduce - and that bitmap is
// copied over the frame. Only rows whose capture differs from the bar's (the split's staged
// writes, a second split) keep an origin of their own, so nothing the game did is flattened.
// The surface is rebuilt only when the state it was built from changes (nametable, palette, CHR
// banks, OAM, the split row or the captured rows), so a steady frame costs one state compare and
// one copy of the bar rows.
//
// The frame is two layers with one boundary between them, not a per-row reconstruction of the
// game's writes. The split IRQ (hud_top) only says where the game *starts* writing the bar's
// scroll; the writes land over the next line or two, and until the first of them lands the row
// still belongs to the playfield. hud_bar_first() finds that row exactly - the first row at or
// below the split that does not continue the row above (other scroll, other fine X, other CHR
// banks) - and everything from there down is the bar's. Rows above it keep their own captured
// scroll, so the ground under the player runs to the level's real last scanline; no row the bar
// owns can ever show level pixels, and no row the level owns can be painted by the bar.
//
// Per-row state still comes from hud_lines[...] for the bar's later rows (a second split keeps
// its own origin instead of being flattened onto the anchor).
//
// Pixel rules mirror output_pixel/sprite_pixel in ppu.cpp (same priority, clipping and
// palettes), but the row is walked per tile instead of per pixel: the nametable tile, its
// attribute and its pattern bytes are fetched once per 8 pixels, and sprites are composited over
// the finished row rather than re-tested at every pixel.

#include "runtime/machine.h"

#include <algorithm>
#include <cstring>

#include "runtime/nes_palette.h"
#include "smb3_syms.h"

namespace {

constexpr int kBarW = 256;

uint8_t reverse_bits(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) r = static_cast<uint8_t>((r << 1) | ((b >> i) & 1));
    return r;
}

// The scroll state one scanline below `v` when nothing is written in between: the coarse X is
// re-copied from t every line, the vertical counter just steps down one row (and wraps the
// nametable at the bottom of the picture).
uint16_t chain_step(uint16_t v) {
    const unsigned fy = (v >> 12) & 7;
    if (fy < 7) return static_cast<uint16_t>(v + 0x1000);
    unsigned y = (v >> 5) & 0x1F, nt = (v >> 11) & 1;
    if (y == 29) {
        y = 0;
        nt ^= 1;
    } else if (y == 31) {
        y = 0;
    } else {
        y += 1;
    }
    return static_cast<uint16_t>((v & ~0x7BE0) | (y << 5) | (nt << 11));
}

// Whether `cur` continues `prev` with no scroll writes between.
bool chain_cont(uint16_t prev, uint16_t cur) { return chain_step(prev) == cur; }

}  // namespace

// Record the state that `line` starts from. Called at dot 1, when the horizontal copy and the
// vertical step of the line before have run and the game's writes from that line's HBlank have landed.
void Machine::hud_capture(int line) {
    HudLine& h = hud_lines[line];
    h.v = v;
    h.fine_x = fine_x;
    h.ctrl = ppuctrl;
    h.mask = ppumask;
    h.on = rendering_enabled();
    // Camera of the frame: the first rendered line's value, before the game advances the scroll
    // again. The wide pass composes after the frame, when Horz_Scroll may already be ahead.
    if (h.on && !frame_scroll_set) {
        frame_scroll = static_cast<uint16_t>(ram[smb3::Horz_Scroll & 0x7FF] |
                                             (ram[smb3::Horz_Scroll_Hi & 0x7FF] << 8));
        frame_scroll_set = true;
    }
    for (size_t i = 0; i < 32; i++) h.palram[i] = palram[i];
    for (size_t i = 0; i < 8; i++) h.chr_slot[i] = chr_slot[i];
    for (size_t i = 0; i < 256; i++) h.oam[i] = oam[i];
}

// The bar's first row: the first row at or below the split that no longer continues the row
// above. The split IRQ only marks where the game *starts* writing the bar's scroll; the writes
// land over the next line or two, and until the first one lands the row is still the playfield.
// Reading the break here is exact - it needs no guess about which rows the split has reached -
// so the level keeps every row up to its last one and the bar owns the rest.
int Machine::hud_bar_first() const {
    for (int y = hud_top; y < kNoHud; y++) {
        const HudLine& h = hud_lines[y];
        if (!h.on) return y;  // rendering off: the per-dot pass draws the backdrop
        if (y == 0) return y;
        const HudLine& above = hud_lines[y - 1];
        if (!above.on) return y;
        // A bank switch is the split's other signal: the bar's tiles live in another CHR bank.
        if (std::memcmp(h.chr_slot, above.chr_slot, sizeof h.chr_slot) != 0) return y;
        if (!chain_cont(above.v, h.v) || h.fine_x != above.fine_x) return y;
    }
    return kNoHud;
}

// First settled bar row: the first y in [hud_top, 240) starting a chain-continuous on-pair.
// The bar's content anchor (its scroll, palette and banks) comes from this row.
int Machine::hud_settled_row() const {
    // The bar's CHR banks are the ones at the bottom of the screen, and the split switches them
    // together with the scroll: requiring them keeps the anchor from landing on a row that is
    // still the playfield (the game's staged writes land over several lines).
    int bottom = 239;
    while (bottom > hud_top && !hud_lines[bottom].on) bottom--;
    for (int y = hud_top; y + 1 < 240; y++) {
        const HudLine &a = hud_lines[y], &b = hud_lines[y + 1];
        if (!a.on || !b.on) continue;
        if (std::memcmp(a.chr_slot, hud_lines[bottom].chr_slot, sizeof a.chr_slot) != 0) continue;
        if (chain_cont(a.v, b.v)) return y;
    }
    return -1;
}

// Whether the cached picture still describes this frame. Compares the state the picture was
// built from, so no mutation path has to remember to invalidate the layer.
bool Machine::hud_matches(int settled, int first) const {
    const HudLayer::Source& s = hud.src;
    if (s.top != hud_top || s.settled != settled || s.first != first) return false;
    const HudLine& a = hud_lines[settled];
    if (s.v != a.v || s.fine_x != a.fine_x) return false;
    if (s.mirror != mirror) return false;
    if (std::memcmp(s.oam, a.oam, sizeof s.oam) != 0) return false;
    if (std::memcmp(s.vram, vram, sizeof s.vram) != 0) return false;
    const int rows = kNoHud - hud_top;
    if (static_cast<int>(s.rows.size()) != rows) return false;
    for (int i = 0; i < rows; i++) {
        const HudLine& h = hud_lines[hud_top + i];
        const HudLayer::Row& r = s.rows[i];
        if (r.v != h.v || r.fine_x != h.fine_x || r.ctrl != h.ctrl || r.mask != h.mask ||
            r.on != (h.on ? 1 : 0)) {
            return false;
        }
        if (std::memcmp(r.palram, h.palram, sizeof r.palram) != 0) return false;
        if (std::memcmp(r.chr_slot, h.chr_slot, sizeof r.chr_slot) != 0) return false;
    }
    return true;
}

void Machine::hud_snapshot(int settled, int first) {
    HudLayer::Source& s = hud.src;
    const HudLine& a = hud_lines[settled];
    s.top = hud_top;
    s.first = first;
    s.settled = settled;
    s.v = a.v;
    s.fine_x = a.fine_x;
    std::memcpy(s.oam, a.oam, sizeof s.oam);
    std::memcpy(s.vram, vram, sizeof s.vram);
    s.mirror = mirror;
    const int rows = kNoHud - hud_top;
    s.rows.resize(rows);
    for (int i = 0; i < rows; i++) {
        const HudLine& h = hud_lines[hud_top + i];
        HudLayer::Row& r = s.rows[i];
        r.v = h.v;
        r.fine_x = h.fine_x;
        r.ctrl = h.ctrl;
        r.mask = h.mask;
        r.on = h.on ? 1 : 0;
        std::memcpy(r.palram, h.palram, sizeof r.palram);
        std::memcpy(r.chr_slot, h.chr_slot, sizeof r.chr_slot);
    }
}

// Draw the whole bar into hud.pixels, in bar-local coordinates.
void Machine::hud_build() {
    const HudLayer::Source& s = hud.src;
    const int rows = kNoHud - s.top;
    hud.rows = rows;
    hud.pixels.assign(static_cast<size_t>(rows) * kBarW, 0);
    hud.on.assign(rows, 0);

    // The bar's origin, in the 512-column two-nametable row: coarse X and nametable from the
    // anchor row, fine scroll from the tail of the bar (the split's staged $2005 write lands a
    // line or two in, so the head of the capture still carries the playfield's). The dot-1
    // capture leads the displayed scroll by the two prefetch bumps (dots 328 and 336 of the line
    // before), so back those 16 pixels off - once, here, instead of once per row.
    int tail = rows - 1;
    while (tail > 0 && !s.rows[tail].on) tail--;
    const unsigned bar_fx = s.rows[tail].fine_x;
    auto column_of = [](uint16_t v, unsigned fx) {
        const unsigned nt_x = (v >> 10) & 1u;
        return (nt_x * 256u + (v & 0x1F) * 8u + fx + 512u - 16u) % 512u;
    };
    const unsigned bar_col = column_of(s.v, bar_fx);

    uint8_t bg_pat[kBarW];
    bool claimed[kBarW];

    // The bar's anchor row: the first row whose capture is the bar's own state. Rows between the
    // boundary and the anchor are the split's transition - the game has written some of the bar's
    // registers but not all of them - so they are drawn from the anchor rather than from their
    // half-written capture, and never show level tiles.
    const int anchor_i = std::clamp(s.settled - s.top, 0, rows - 1);

    for (int i = 0; i < rows; i++) {
        if (!s.rows[i].on) continue;  // rendering was off: the per-dot pass already drew this row
        hud.on[i] = 1;

        // The two layers meet at s.first. Above it the rows continue the playfield - the split's
        // staged $2006/$2005 writes have not landed yet - so they keep their own captured scroll
        // and are drawn as the level has them; the ground under the player therefore runs to the
        // level's real last scanline. From s.first down the rows are the bar's: drawn from the
        // anchor until the capture is the bar's own (the anchor row), then from their own.
        const int y = s.top + i;
        const bool own = y < s.first || i >= anchor_i;
        const HudLayer::Row& r = s.rows[own ? i : anchor_i];
        const uint16_t v = own ? r.v : s.v;
        const uint8_t row_fx = own ? r.fine_x : bar_fx;
        const unsigned ty = (v >> 5) & 0x1F;
        const unsigned nty = (v >> 11) & 1;
        const unsigned fy = (v >> 12) & 7;

        const unsigned bg_base = (r.ctrl & 0x10) ? 0x1000u : 0x0000u;
        const uint8_t color_mask = (r.mask & 0x01) ? 0x30 : 0x3F;
        const bool bg_on = (r.mask & 0x08) != 0;
        const bool bg_left_on = (r.mask & 0x02) != 0;
        const bool spr_on = (r.mask & 0x10) != 0;
        const bool spr_left_on = (r.mask & 0x04) != 0;
        const bool tall = (r.ctrl & 0x20) != 0;
        const unsigned spr_base = (r.ctrl & 0x08) ? 0x1000u : 0x0000u;
        auto chr_byte = [this, &r](unsigned addr) {
            return chr[static_cast<size_t>(r.chr_slot[(addr >> 10) & 7]) * 0x400 + (addr & 0x3FF)];
        };
        auto color_of = [&r, color_mask](unsigned index) {
            return nes_rgba(r.palram[Machine::pal_index(static_cast<uint16_t>(0x3F00 + index))] &
                            color_mask);
        };

        uint32_t* out = &hud.pixels[static_cast<size_t>(i) * kBarW];

        // A row whose captured horizontal state is not the bar's - another split lower down -
        // keeps its own origin instead of being sheared onto the bar's.
        const unsigned col = ((v & 0x041F) == (s.v & 0x041F) && row_fx == bar_fx)
                                 ? bar_col
                                 : column_of(v, row_fx);

        // Background, one nametable tile at a time.
        unsigned wx = col;
        unsigned pal2 = 0, lo = 0, hi = 0;
        bool loaded = false;
        for (int x = 0; x < kBarW; x++) {
            const unsigned fx = wx & 7u;
            if (!loaded || fx == 0) {
                const unsigned ntx = wx >> 8;
                const unsigned tx = (wx & 0xFFu) >> 3;
                const unsigned quad = (nty << 1) | ntx;
                const uint8_t tile =
                    vram[nt_index(static_cast<uint16_t>(0x2000 | (quad << 10) | (ty * 32u) | tx))];
                const uint8_t attr =
                    vram[nt_index(static_cast<uint16_t>(0x23C0 | (quad << 10) | ((ty >> 2) << 3) |
                                                       (tx >> 2)))];
                pal2 = (attr >> (((ty & 2) << 1) | (tx & 2))) & 3u;
                const unsigned pat_addr = bg_base + 16u * tile + fy;
                lo = chr_byte(pat_addr);
                hi = chr_byte(pat_addr + 8);
                loaded = true;
            }
            const unsigned bit = 7u - fx;
            const unsigned pat = (((hi >> bit) & 1u) << 1) | ((lo >> bit) & 1u);
            bg_pat[x] = static_cast<uint8_t>(pat);
            const bool visible = bg_on && !(x < 8 && !bg_left_on);
            out[x] = color_of((visible && pat) ? ((pal2 << 2) | pat) : 0u);
            wx = (wx + 1) & 511u;
        }

        // Sprites, evaluated for this row from the bar's snapshot and composited over the row.
        // Row y of a sprite selects (y-1-sy): the patterns were fetched during line y-1, so the
        // first row of the screen has no sprites, as on the hardware.
        if (!spr_on) continue;
        struct BarSpr {
            uint8_t lo, hi, x, pal;
            bool behind;
        } sprs[8];
        int nsprs = 0;
        for (int k = 0; k < 64 && nsprs < 8; k++) {
            const unsigned sy = s.oam[k * 4], tile = s.oam[k * 4 + 1], attr = s.oam[k * 4 + 2];
            const unsigned height = tall ? 16u : 8u;
            const unsigned diff = static_cast<unsigned>(y - 1) - sy;
            if (diff >= height) continue;
            const unsigned row = (attr & 0x80) ? (height - 1 - diff) : diff;
            unsigned addr;
            if (!tall) {
                addr = spr_base + 16u * tile + (row & 7);
            } else {
                addr = ((tile & 1) ? 0x1000u : 0x0000u) + 16u * (tile & 0xFE) +
                       ((row & 8) << 1) + (row & 7);
            }
            BarSpr sp;
            sp.lo = chr_byte(addr);
            sp.hi = chr_byte(addr + 8);
            if (attr & 0x40) {
                sp.lo = reverse_bits(sp.lo);
                sp.hi = reverse_bits(sp.hi);
            }
            sp.x = s.oam[k * 4 + 3];
            sp.pal = attr & 3;
            sp.behind = (attr & 0x20) != 0;
            sprs[nsprs++] = sp;
        }
        if (nsprs == 0) continue;
        std::memset(claimed, 0, sizeof claimed);
        for (int k = 0; k < nsprs; k++) {
            const BarSpr& sp = sprs[k];
            const int x0 = std::max(0, static_cast<int>(sp.x));
            const int x1 = std::min(kBarW, static_cast<int>(sp.x) + 8);
            for (int x = x0; x < x1; x++) {
                if (claimed[x]) continue;
                if (x < 8 && !spr_left_on) continue;
                const unsigned off = static_cast<unsigned>(x - sp.x);
                const unsigned shift = 7u - off;
                const unsigned pat = (((sp.hi >> shift) & 1u) << 1) | ((sp.lo >> shift) & 1u);
                if (!pat) continue;
                claimed[x] = true;  // first sprite with a pixel wins, as in sprite_pixel
                if (sp.behind && bg_pat[x]) continue;
                out[x] = color_of(0x10u + (static_cast<unsigned>(sp.pal) << 2) + pat);
            }
        }
    }
}

// Composite the bar over the frame: copy the cached rows. Nothing is drawn above hud_top - the
// rows there are the level's own, drawn by the per-dot pass.
void Machine::hud_blit() {
    const int top = hud.src.top;
    if (top < 0 || hud.rows <= 0) return;
    int i = 0;
    while (i < hud.rows) {
        while (i < hud.rows && !hud.on[i]) i++;
        const int first = i;
        while (i < hud.rows && hud.on[i]) i++;
        if (i > first) {
            std::memcpy(&framebuf[static_cast<size_t>(top + first) * 256],
                        &hud.pixels[static_cast<size_t>(first) * 256],
                        static_cast<size_t>(i - first) * 256 * sizeof(uint32_t));
        }
    }
}

void Machine::hud_compose() {
    if (hud_top >= kNoHud) {
        hud.rows = 0;
        hud.src.top = -1;
        hud.src.first = -1;
        return;
    }
    int first = hud_bar_first();
    if (first < hud_top || first >= kNoHud) first = hud_top;  // no break found: split at the IRQ
    int settled = hud_settled_row();
    if (settled < first) settled = first;  // anchor on a row the bar actually owns
    if (!hud_matches(settled, first)) {
        hud_snapshot(settled, first);
        hud_build();
    }
    hud_blit();
}
