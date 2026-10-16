// PPU rendering for one scanline, modelled dot by dot after nesalizer's ppu.cpp
// (https://github.com/ulfalizer/nesalizer): background shift registers, sprite
// evaluation and loading, the v/t scroll registers with their bump and copy rules,
// and the sprite-0 hit. Fetch sequencing cross-checked against MesenCE's NesPpu
// (https://github.com/nesdev-org/MesenCE, GPLv3). Rewritten; no MesenCE code.
// The CPU runs in chunks between scanlines, so register writes
// made during a line take effect from the next line on.
#include "runtime/machine.h"

#include <cstring>

#include "runtime/nes_palette.h"

namespace {

constexpr int kPrerenderLine = 261;

void bump_horiz(Machine& m) {
    if ((m.v & 0x001F) == 31) {
        m.v = static_cast<uint16_t>(m.v & ~0x001F);
        m.v = static_cast<uint16_t>(m.v ^ 0x0400);
    } else {
        m.v = static_cast<uint16_t>(m.v + 1);
    }
}

// One vertical scroll step (the v register's rule at dot 256). Returned so the HUD can step its own copy.
uint16_t bump_vert_v(uint16_t v) {
    if ((v & 0x7000) != 0x7000) return static_cast<uint16_t>(v + 0x1000);
    v = static_cast<uint16_t>(v & ~0x7000);
    int y = (v & 0x03E0) >> 5;
    if (y == 29) {
        y = 0;
        v = static_cast<uint16_t>(v ^ 0x0800);
    } else if (y == 31) {
        y = 0;
    } else {
        y += 1;
    }
    return static_cast<uint16_t>((v & ~0x03E0) | (y << 5));
}

void bump_vert(Machine& m) { m.v = bump_vert_v(m.v); }

// Whether cur continues prev with no scroll writes between: same horizontal scroll
// (re-copied from t every line while rendering) plus one vertical bump.
bool hud_chain_cont(uint16_t prev, uint16_t cur) {
    if ((prev & 0x041F) != (cur & 0x041F)) return false;  // coarse X + horizontal nametable
    const unsigned fy = (prev >> 12) & 7;
    if (fy < 7) return cur == static_cast<uint16_t>(prev + 0x1000);
    unsigned y = (prev >> 5) & 0x1F, nt = (prev >> 11) & 1;
    if (y == 29) {
        y = 0;
        nt ^= 1;
    } else if (y == 31) {
        y = 0;
    } else {
        y += 1;
    }
    return cur == static_cast<uint16_t>((prev & ~0x7BE0) | (y << 5) | (nt << 11));
}

void copy_horiz(Machine& m) {
    m.v = static_cast<uint16_t>((m.v & ~0x041F) | (m.t & 0x041F));
}

void copy_vert(Machine& m) {
    m.v = static_cast<uint16_t>((m.v & ~0x7BE0) | (m.t & 0x7BE0));
}

// Background fetches, one tile every 8 dots. phase = (dot - 1) % 8.
void bg_fetch(Machine& m, int phase) {
    const unsigned bg_pat = (m.ppuctrl & 0x10) ? 0x1000u : 0x0000u;
    switch (phase) {
        case 0:
            m.ppu_set_addr(static_cast<uint16_t>(0x2000 | (m.v & 0x0FFF)));
            break;
        case 1:
            m.nt_byte = m.ppu_mem_read(m.ppu_addr_bus);
            break;
        case 2:
            m.ppu_set_addr(static_cast<uint16_t>(0x23C0 | (m.v & 0x0C00) | ((m.v >> 4) & 0x38) | ((m.v >> 2) & 0x07)));
            break;
        case 3:
            // Select this tile's 2-bit palette quadrant now, from v before the coarse X bump
            // on dot 8. Reloading later would read the next tile's quadrant.
            m.at_byte = static_cast<uint8_t>(m.ppu_mem_read(m.ppu_addr_bus) >> (((m.v >> 4) & 4) | (m.v & 2)));
            break;
        case 4:
            m.ppu_set_addr(static_cast<uint16_t>(bg_pat + 16u * m.nt_byte + ((m.v >> 12) & 7)));
            break;
        case 5:
            m.bg_byte_l = m.chr_read(m.ppu_addr_bus);
            break;
        case 6:
            m.ppu_set_addr(static_cast<uint16_t>(m.ppu_addr_bus + 8));
            break;
        case 7:
            m.bg_byte_h = m.chr_read(m.ppu_addr_bus);
            bump_horiz(m);
            break;
    }
}

// Shift the background and attribute registers one dot. Reload on dots 9, 17, ... 257.
void shift_and_reload(Machine& m, int dot) {
    m.bg_shift_l = static_cast<uint16_t>(m.bg_shift_l << 1);
    m.bg_shift_h = static_cast<uint16_t>(m.bg_shift_h << 1);
    m.at_shift_l = (m.at_shift_l << 1) | m.at_latch_l;
    m.at_shift_h = (m.at_shift_h << 1) | m.at_latch_h;
    if (dot % 8 == 1) {
        m.bg_shift_l = static_cast<uint16_t>((m.bg_shift_l & 0xFF00) | m.bg_byte_l);
        m.bg_shift_h = static_cast<uint16_t>((m.bg_shift_h & 0xFF00) | m.bg_byte_h);
        m.at_latch_l = m.at_byte & 1;
        m.at_latch_h = (m.at_byte >> 1) & 1;
    }
}

uint8_t reverse_bits(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) r = static_cast<uint8_t>((r << 1) | ((b >> i) & 1));
    return r;
}

void next_oam_byte(Machine& m) {
    m.oamaddr = static_cast<uint8_t>(m.oamaddr + 1);
    m.sec_oam_addr = static_cast<uint8_t>((m.sec_oam_addr + 1) & 0x1F);
    if (m.oamaddr == 0) m.oam_addr_overflow = true;
    if (m.sec_oam_addr == 0) {
        m.sec_oam_addr_overflow = true;
        m.overflow_detection = true;
    }
}

// Sprite evaluation for the next line, on dots 65-256 of a visible line.
void sprite_eval(Machine& m, int line, int dot) {
    if (dot == 65) {
        m.overflow_detection = m.oam_addr_overflow = m.sec_oam_addr_overflow = false;
        m.sec_oam_addr = 0;
    }
    if (dot & 1) {
        m.oam_data = m.oam[m.oamaddr];
        return;
    }
    const uint8_t orig = m.oam_data;
    if (!(m.oam_addr_overflow || m.sec_oam_addr_overflow)) m.sec_oam[m.sec_oam_addr] = m.oam_data;
    else m.oam_data = m.sec_oam[m.sec_oam_addr];

    if (m.copy_sprite_signal > 0) {
        m.copy_sprite_signal--;
        next_oam_byte(m);
        return;
    }
    const unsigned height = (m.ppuctrl & 0x20) ? 16u : 8u;
    const bool in_range = (static_cast<unsigned>(line) - orig) < height;
    if (dot == 66) m.s0_next = in_range;

    if (in_range && !(m.oam_addr_overflow || m.sec_oam_addr_overflow)) {
        m.copy_sprite_signal = 3;
        next_oam_byte(m);
        return;
    }
    if (!m.overflow_detection) {
        m.oamaddr = static_cast<uint8_t>((m.oamaddr + 4) & 0xFC);
        if (m.oamaddr == 0) m.oam_addr_overflow = true;
    } else {
        if (in_range && !m.oam_addr_overflow) {
            m.sprite_overflow = true;
            m.overflow_detection = false;
        } else {
            m.oamaddr = static_cast<uint8_t>(((m.oamaddr + 4) & 0xFC) | ((m.oamaddr + 1) & 3));
            if ((m.oamaddr & 0xFC) == 0) m.oam_addr_overflow = true;
        }
    }
}

// Pattern-table address of one sprite row. Returns whether the sprite covers the line.
bool sprite_tile_addr(Machine& m, int line, uint8_t y, uint8_t index, uint8_t attrib, bool high) {
    const unsigned diff = static_cast<unsigned>(line) - y;
    const unsigned diff_flip = (attrib & 0x80) ? ~diff : diff;
    if (!(m.ppuctrl & 0x20)) {
        const unsigned base = (m.ppuctrl & 0x08) ? 0x1000u : 0x0000u;
        m.ppu_set_addr(static_cast<uint16_t>(base + 16u * index + (high ? 8u : 0u) + (diff_flip & 7)));
        return diff < 8;
    }
    const unsigned base = (index & 1) ? 0x1000u : 0x0000u;
    m.ppu_set_addr(static_cast<uint16_t>(base + 16u * (index & 0xFE) + ((diff_flip & 8) << 1) +
                                           (high ? 8u : 0u) + (diff_flip & 7)));
    return diff < 16;
}

// Sprite loading from secondary OAM, on dots 257-320 (fetches for the next line).
void sprite_load(Machine& m, int line, int dot) {
    const int n = (dot - 257) / 8;
    if (dot == 257) m.sec_oam_addr = 0;
    m.s0_cur = m.s0_next;
    switch ((dot - 1) % 8) {
        case 0:
            m.ppu_set_addr(static_cast<uint16_t>(0x2000 | (m.v & 0x0FFF)));
            m.sprite_y = m.sec_oam[m.sec_oam_addr];
            m.sec_oam_addr = static_cast<uint8_t>((m.sec_oam_addr + 1) & 0x1F);
            break;
        case 1:
            m.sprite_index = m.sec_oam[m.sec_oam_addr];
            m.sec_oam_addr = static_cast<uint8_t>((m.sec_oam_addr + 1) & 0x1F);
            break;
        case 2:
            m.ppu_set_addr(static_cast<uint16_t>(0x2000 | (m.v & 0x0FFF)));
            m.sprite_attribs[n] = m.sec_oam[m.sec_oam_addr];
            m.sec_oam_addr = static_cast<uint8_t>((m.sec_oam_addr + 1) & 0x1F);
            break;
        case 3:
            m.sprite_x[n] = m.sec_oam[m.sec_oam_addr];
            m.sec_oam_addr = static_cast<uint8_t>((m.sec_oam_addr + 1) & 0x1F);
            break;
        case 4:
            m.sprite_in_range = sprite_tile_addr(m, line, m.sprite_y, m.sprite_index, m.sprite_attribs[n], false);
            break;
        case 5:
            m.sprite_pat_l[n] = m.sprite_in_range ? m.chr_read(m.ppu_addr_bus) : 0;
            if (m.sprite_attribs[n] & 0x40) m.sprite_pat_l[n] = reverse_bits(m.sprite_pat_l[n]);
            break;
        case 6:
            m.sprite_in_range = sprite_tile_addr(m, line, m.sprite_y, m.sprite_index, m.sprite_attribs[n], true);
            break;
        case 7:
            m.sprite_pat_h[n] = m.sprite_in_range ? m.chr_read(m.ppu_addr_bus) : 0;
            if (m.sprite_attribs[n] & 0x40) m.sprite_pat_h[n] = reverse_bits(m.sprite_pat_h[n]);
            break;
    }
}

// Sprite pixel at x. Returns the 2-bit pattern (0 = none) and sets palette, priority, sprite-0 flag.
unsigned sprite_pixel(Machine& m, int x, unsigned& pal, bool& behind, bool& is_s0) {
    if (x < 8 && !(m.ppumask & 0x04)) return 0;
    for (unsigned i = 0; i < 8; i++) {
        const int offset = x - m.sprite_x[i];
        if (offset < 0 || offset >= 8) continue;
        const unsigned shift = static_cast<unsigned>(7 - offset);
        const unsigned pat = (((m.sprite_pat_h[i] >> shift) & 1) << 1) | ((m.sprite_pat_l[i] >> shift) & 1);
        if (pat == 0) continue;
        pal = m.sprite_attribs[i] & 3;
        behind = (m.sprite_attribs[i] & 0x20) != 0;
        is_s0 = m.s0_cur && i == 0;
        return pat;
    }
    return 0;
}

// Output the pixel for dot 2-257 of a visible line.
void output_pixel(Machine& m, int line, int dot) {
    const int x = dot - 2;
    unsigned pal_index;
    if (!m.rendering_enabled()) {
        pal_index = (~m.v & 0x3F00) ? 0 : (m.v & 0x1F);
    } else {
        unsigned bg_pat = 0;
        if ((m.ppumask & 0x08) && !(x < 8 && !(m.ppumask & 0x02))) {
            bg_pat = (((m.bg_shift_h >> (15 - m.fine_x)) & 1) << 1) | ((m.bg_shift_l >> (15 - m.fine_x)) & 1);
        }
        unsigned spr_pal = 0;
        bool spr_behind = false, spr_s0 = false;
        const unsigned spr = sprite_pixel(m, x, spr_pal, spr_behind, spr_s0);
        if (spr && spr_s0 && bg_pat && x != 255) m.sprite0_hit = true;

        if (spr && !(spr_behind && bg_pat)) {
            pal_index = 0x10u + (spr_pal << 2) + spr;
        } else if (!bg_pat) {
            pal_index = 0;
        } else {
            const unsigned attr = (((m.at_shift_h >> (7 - m.fine_x)) & 1) << 1) | ((m.at_shift_l >> (7 - m.fine_x)) & 1);
            pal_index = (attr << 2) | bg_pat;
        }
    }
    const uint8_t mask = (m.ppumask & 0x01) ? 0x30 : 0x3F;
    const uint8_t color = m.palram[Machine::pal_index(static_cast<uint16_t>(0x3F00 + pal_index))] & mask;
    m.framebuf[static_cast<size_t>(line) * 256 + x] = nes_rgba(color);
}

// Rendering operations on the visible and pre-render lines (rendering enabled).
void render_ops(Machine& m, int line, int dot) {
    if ((dot >= 2 && dot <= 257) || (dot >= 322 && dot <= 337)) shift_and_reload(m, dot);
    if ((dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 336)) {
        bg_fetch(m, (dot - 1) % 8);
        if (dot == 256) bump_vert(m);
    } else if (dot >= 257 && dot <= 320) {
        sprite_load(m, line, dot);
        m.oamaddr = 0;
        if (dot == 257) copy_horiz(m);
    } else if (dot == 337 || dot == 339) {
        m.ppu_set_addr(static_cast<uint16_t>(0x2000 | (m.v & 0x0FFF)));
    }
}

}  // namespace

void Machine::render_dot(int line, int dot) {
    const bool visible = line < 240;
    const bool prerender = line == kPrerenderLine;
    // A $2006 write reaches v three dots after it lands. No CPU write can change t in that window.
    if (pending_v_update > 0 && --pending_v_update == 0) v = t;
    if (visible && dot >= 2 && dot <= 257) output_pixel(*this, line, dot);
    if (!(visible || prerender) || !rendering_enabled()) return;

    render_ops(*this, line, dot);
    if (visible) {
        if (dot >= 1 && dot <= 64) {
            if (dot & 1) {
                oam_data = 0xFF;
            } else {
                sec_oam[sec_oam_addr] = oam_data;
                sec_oam_addr = static_cast<uint8_t>((sec_oam_addr + 1) & 0x1F);
            }
        } else if (dot >= 65 && dot <= 256) {
            sprite_eval(*this, line, dot);
        }
    } else {
        if (dot == 1) sprite_overflow = false;
        if (dot == 66) s0_next = false;
        if (dot >= 280 && dot <= 304) copy_vert(*this);
    }
}

// Record the state that `line` starts from. Called at dot 1, when the horizontal copy and the
// vertical step of the line before have run and the game's writes from that line's HBlank have landed.
void Machine::hud_capture(int line) {
    HudLine& h = hud_lines[line];
    h.v = v;
    h.fine_x = fine_x;
    h.ctrl = ppuctrl;
    h.mask = ppumask;
    h.on = rendering_enabled();
    for (size_t i = 0; i < 32; i++) h.palram[i] = palram[i];
    for (size_t i = 0; i < 8; i++) h.chr_slot[i] = chr_slot[i];
    for (size_t i = 0; i < 256; i++) h.oam[i] = oam[i];
}

// Draw rows hud_top..239 from the captured state, as a layer separate from the game: the per-dot
// pass draws these rows with the playfield scroll (the split writes have not landed yet), so the
// compositor redraws them here from each row's own scroll, palette, banks and sprites. Redrawing the
// whole row from a per-row snapshot keeps the bar stable even when the IRQ position moves a line.
// The split writes land over several lines, so the first rows may hold mid-split transient scrolls
// (a chain break vs the previous row); those are redrawn from the first settled row's state, so no
// transient-colour line shows above the bar. The composed top row is then duplicated one scanline
// up, overlaying the gameplay.
// Pixel rules mirror output_pixel/sprite_pixel above (same priority, clipping and palettes).
void Machine::hud_compose() {
    if (hud_top >= kNoHud) return;
    // First settled row: first y in [hud_top, 240) starting a chain-continuous on-pair.
    int settled = -1;
    for (int y = hud_top; y + 1 < 240; y++) {
        const HudLine &a = hud_lines[y], &b = hud_lines[y + 1];
        if (!a.on || !b.on) continue;
        if (hud_chain_cont(a.v, b.v)) {
            settled = y;
            break;
        }
    }
    const HudLine* hs = settled >= 0 ? &hud_lines[settled] : nullptr;
    auto draw_row = [this](int y, const HudLine& h, uint16_t v_eff) {
        const unsigned bg_base = (h.ctrl & 0x10) ? 0x1000u : 0x0000u;
        const unsigned color_mask = (h.mask & 0x01) ? 0x30 : 0x3F;
        const bool bg_on = (h.mask & 0x08) != 0;
        const bool bg_left_on = (h.mask & 0x02) != 0;
        const bool spr_on = (h.mask & 0x10) != 0;
        const bool spr_left_on = (h.mask & 0x04) != 0;
        const unsigned ty = (v_eff >> 5) & 0x1F;
        const unsigned nty = (v_eff >> 11) & 1;
        const unsigned fy = (v_eff >> 12) & 7;
        const unsigned nt_x = (v_eff >> 10) & 1u;
        // The dot-1 snapshot leads the displayed scroll by the two prefetch bumps (dots 328 and
        // 336 of the previous line): the row's first fetches already used v two tiles ahead, so the
        // tile under screen pixel x is two columns before the snapshot's. Back those 16 pixels off
        // (mod the 512-wide two-nametable row) to recover the true scroll for this row.
        const unsigned start =
            (nt_x * 256u + (v_eff & 0x1F) * 8u + h.fine_x + 512u - 16u) % 512u;
        auto chr_byte = [this, &h](unsigned addr) {
            return chr[static_cast<size_t>(h.chr_slot[(addr >> 10) & 7]) * 0x400 + (addr & 0x3FF)];
        };
        // Sprite evaluation for this row from the captured OAM: first 8 sprites in OAM order that
        // cover the row, with their pattern bytes fetched now (flips applied). Absolute-positioned,
        // so they must be redrawn here or the background redraw would erase them.
        struct HudSpr { uint8_t lo = 0, hi = 0, x = 0, pal = 0; bool behind = false; } sprs[8];
        int nsprs = 0;
        if (spr_on) {
            const bool tall = (h.ctrl & 0x20) != 0;
            const unsigned spr_base = (h.ctrl & 0x08) ? 0x1000u : 0x0000u;
            for (int i = 0; i < 64 && nsprs < 8; i++) {
                const unsigned sy = h.oam[i * 4], tile = h.oam[i * 4 + 1], attr = h.oam[i * 4 + 2];
                const unsigned height = tall ? 16u : 8u;
                // Sprites on row y were evaluated during line y-1 for rule y-1 and their patterns
                // fetched in y-1's HBlank, so the row selects (y-1-sy), one less than the
                // background's row. Row 0 therefore never has sprites, as on the hardware.
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
                HudSpr s;
                s.lo = chr_byte(addr);
                s.hi = chr_byte(addr + 8);
                if (attr & 0x40) {
                    s.lo = reverse_bits(s.lo);
                    s.hi = reverse_bits(s.hi);
                }
                s.x = h.oam[i * 4 + 3];
                s.pal = attr & 3;
                s.behind = (attr & 0x20) != 0;
                sprs[nsprs++] = s;
            }
        }
        for (int x = 0; x < 256; x++) {
            // Horizontal: 512 columns across two nametables, starting from this line's scroll.
            const unsigned wx = (start + static_cast<unsigned>(x)) % 512u;
            const unsigned ntx = wx >> 8;
            const unsigned tx = (wx & 0xFF) / 8u;
            const unsigned fx = wx & 7u;
            const unsigned quad = (nty << 1) | ntx;
            const uint8_t tile = vram[nt_index(static_cast<uint16_t>(0x2000 | (quad << 10) | (ty * 32u) | tx))];
            const uint8_t attr = vram[nt_index(static_cast<uint16_t>(0x23C0 | (quad << 10) | ((ty >> 2) << 3) | (tx >> 2)))];
            const unsigned pal2 = (attr >> (((ty & 2) << 1) | (tx & 2))) & 3u;
            const unsigned pat_addr = bg_base + 16u * tile + fy;
            const unsigned lo = chr_byte(pat_addr);
            const unsigned hi = chr_byte(pat_addr + 8);
            const unsigned bit = 7u - fx;
            const unsigned bg_pat = (((hi >> bit) & 1u) << 1) | ((lo >> bit) & 1u);
            const bool bg_visible = bg_on && !(x < 8 && !bg_left_on);
            unsigned idx = (bg_visible && bg_pat) ? ((pal2 << 2) | bg_pat) : 0u;
            if (nsprs > 0 && (x >= 8 || spr_left_on)) {
                for (int i = 0; i < nsprs; i++) {
                    const int off = x - sprs[i].x;
                    if (off < 0 || off >= 8) continue;
                    const unsigned shift = static_cast<unsigned>(7 - off);
                    const unsigned spat = (((sprs[i].hi >> shift) & 1u) << 1) | ((sprs[i].lo >> shift) & 1u);
                    if (!spat) continue;
                    if (!(sprs[i].behind && bg_pat)) idx = 0x10u + (sprs[i].pal << 2) + spat;
                    break;  // first (lowest-index) covering sprite wins, as in sprite_pixel
                }
            }
            const uint8_t color = h.palram[Machine::pal_index(static_cast<uint16_t>(0x3F00 + idx))] & color_mask;
            framebuf[static_cast<size_t>(y) * 256 + x] = nes_rgba(color);
        }
    };
    for (int y = hud_top; y < 240; y++) {
        const HudLine& h = hud_lines[y];
        if (!h.on) continue;  // rendering was off: the per-dot pass already drew this row
        const bool chained = y > 0 && hud_lines[y - 1].on && hud_chain_cont(hud_lines[y - 1].v, h.v);
        if (hs && y < settled && !chained) {
            // Mid-split transient scroll (the staged $2006/$2005 writes land over several
            // lines, so this snapshot holds an intermediate v such as the zeroed one): draw
            // the settled row's content instead, so no transient-colour line shows above
            // the bar. Same v (not stepped back): the scroll addresses the content, and the
            // settled scroll is what these rows should display.
            draw_row(y, *hs, hs->v);
        } else {
            draw_row(y, h, h.v);
        }
    }
    // Extend the bar one scanline upward: duplicate the composed top row over the gameplay
    // row above it. A pixel copy (not a re-render) so the bar's top edge covers whatever
    // the per-dot pass drew there, including any split-position jitter.
    if (hud_top > 0 && hud_lines[hud_top].on) {
        std::memcpy(&framebuf[static_cast<size_t>(hud_top - 1) * 256],
                    &framebuf[static_cast<size_t>(hud_top) * 256], 256 * sizeof(uint32_t));
    }
}

void Machine::ppu_data_bump(bool rendering_line) {
    if (rendering_enabled() && rendering_line) {
        bump_horiz(*this);
        bump_vert(*this);
    } else {
        v = static_cast<uint16_t>(v + ((ppuctrl & 0x04) ? 32 : 1));
    }
}
