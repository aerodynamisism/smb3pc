// PPU rendering for one scanline, modelled dot by dot after nesalizer's ppu.cpp
// (https://github.com/ulfalizer/nesalizer): background shift registers, sprite
// evaluation and loading, the v/t scroll registers with their bump and copy rules,
// and the sprite-0 hit. The CPU runs in chunks between scanlines, so register writes
// made during a line take effect from the next line on.
#include "runtime/machine.h"

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

void bump_vert(Machine& m) {
    if ((m.v & 0x7000) != 0x7000) {
        m.v = static_cast<uint16_t>(m.v + 0x1000);
        return;
    }
    m.v = static_cast<uint16_t>(m.v & ~0x7000);
    int y = (m.v & 0x03E0) >> 5;
    if (y == 29) {
        y = 0;
        m.v = static_cast<uint16_t>(m.v ^ 0x0800);
    } else if (y == 31) {
        y = 0;
    } else {
        y += 1;
    }
    m.v = static_cast<uint16_t>((m.v & ~0x03E0) | (y << 5));
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
            m.ppu_addr_bus = static_cast<uint16_t>(0x2000 | (m.v & 0x0FFF));
            break;
        case 1:
            m.nt_byte = m.ppu_mem_read(m.ppu_addr_bus);
            break;
        case 2:
            m.ppu_addr_bus = static_cast<uint16_t>(0x23C0 | (m.v & 0x0C00) | ((m.v >> 4) & 0x38) | ((m.v >> 2) & 0x07));
            break;
        case 3:
            // Select this tile's 2-bit palette quadrant now, from v before the coarse X bump
            // on dot 8. Reloading later would read the next tile's quadrant.
            m.at_byte = static_cast<uint8_t>(m.ppu_mem_read(m.ppu_addr_bus) >> (((m.v >> 4) & 4) | (m.v & 2)));
            break;
        case 4:
            m.ppu_addr_bus = static_cast<uint16_t>(bg_pat + 16u * m.nt_byte + ((m.v >> 12) & 7));
            break;
        case 5:
            m.bg_byte_l = m.chr_read(m.ppu_addr_bus);
            break;
        case 6:
            m.ppu_addr_bus = static_cast<uint16_t>(m.ppu_addr_bus + 8);
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
        m.ppu_addr_bus = static_cast<uint16_t>(base + 16u * index + (high ? 8u : 0u) + (diff_flip & 7));
        return diff < 8;
    }
    const unsigned base = (index & 1) ? 0x1000u : 0x0000u;
    m.ppu_addr_bus = static_cast<uint16_t>(base + 16u * (index & 0xFE) + ((diff_flip & 8) << 1) +
                                           (high ? 8u : 0u) + (diff_flip & 7));
    return diff < 16;
}

// Sprite loading from secondary OAM, on dots 257-320 (fetches for the next line).
void sprite_load(Machine& m, int line, int dot) {
    const int n = (dot - 257) / 8;
    if (dot == 257) m.sec_oam_addr = 0;
    m.s0_cur = m.s0_next;
    switch ((dot - 1) % 8) {
        case 0:
            m.ppu_addr_bus = static_cast<uint16_t>(0x2000 | (m.v & 0x0FFF));
            m.sprite_y = m.sec_oam[m.sec_oam_addr];
            m.sec_oam_addr = static_cast<uint8_t>((m.sec_oam_addr + 1) & 0x1F);
            break;
        case 1:
            m.sprite_index = m.sec_oam[m.sec_oam_addr];
            m.sec_oam_addr = static_cast<uint8_t>((m.sec_oam_addr + 1) & 0x1F);
            break;
        case 2:
            m.ppu_addr_bus = static_cast<uint16_t>(0x2000 | (m.v & 0x0FFF));
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
        m.ppu_addr_bus = static_cast<uint16_t>(0x2000 | (m.v & 0x0FFF));
    }
}

}  // namespace

void Machine::render_line(int line) {
    const bool visible = line < 240;
    const bool prerender = line == kPrerenderLine;
    for (int dot = 0; dot < 341; dot++) {
        if (pending_v_update > 0 && --pending_v_update == 0) v = t;
        if (visible && dot >= 2 && dot <= 257) output_pixel(*this, line, dot);
        if (!(visible || prerender) || !rendering_enabled()) {
            continue;
        }
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
}

void Machine::ppu_data_bump(bool rendering_line) {
    if (rendering_enabled() && rendering_line) {
        bump_horiz(*this);
        bump_vert(*this);
    } else {
        v = static_cast<uint16_t>(v + ((ppuctrl & 0x04) ? 32 : 1));
    }
}
