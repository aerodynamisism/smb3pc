#include "runtime/machine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Generated table: g_bank_fns[bank] = bank_NN (or nullptr for data-only banks).
#include "bank_table.inc"
#include "smb3_syms.h"

namespace {
constexpr int kLinesPerFrame = 262;
constexpr int kVisibleLines = 240;
constexpr int kVblankLine = 241;
constexpr int kPrerenderLine = 261;
constexpr int kDotsPerLine = 341;
constexpr int64_t kA12FilterDots = 9;  // A12 must stay low this long before a rise clocks the MMC3
}  // namespace

// ---------------------------------------------------------------- lifecycle

void Machine::reset() {
    apu.read_cpu = [this](uint16_t addr) { return rd(addr); };  // DMC sample fetches
    apu.reset();  // linear Blargg APU to power-on state (keeps read_cpu hook)
    sfx_apu.read_cpu = apu.read_cpu;
    sfx_apu.reset();
    // Sprite Y records: no drawer has computed anything until the first OAM DMA snapshots the
    // set, and an empty set is "the frame has no recorded sprites".
    std::memset(spr_y_rec, kSprYNone, sizeof spr_y_rec);
    std::memset(spr_y_frame, kSprYNone, sizeof spr_y_frame);
    spr_y_seen = 0;
    // The game enables its channels once at startup, through the music voice set. Effects need the
    // same enables on their own set, or a note written before the engine's first $4015 write is silent.
    sfx_apu.write(0x4015, 0x0F, 0);
    snd_owner = kSndMusic;
    for (auto& o : snd_sav) o = kSndMusic;
    prg_mode = 0;
    chr_inv = 0;
    mmc3_sel = 0;
    for (auto& r : mmc3_r) r = 0;
    update_mmc3_banks();
    s = 0xFD;
    p = 0x24;
    cyc = 0;
    evt = 0;
    ppu_dot_pos = 0;
    ppu_line = 0;
    ppu_dot = 0;
    pending_v_update = 0;
    a12_low_dot = -1;
    irq_line = false;
    nmi_pending = false;
    pc = rd16(0xFFFC);
}

// ---------------------------------------------------------------- I/O space

uint8_t Machine::rd_io(uint16_t addr) {
    if (addr < 0x4000) {
        ppu_sync();
        switch (addr & 7) {
            case 2: {  // PPUSTATUS
                uint8_t st = static_cast<uint8_t>((vblank ? 0x80 : 0) | (sprite0_hit ? 0x40 : 0) |
                                                  (sprite_overflow ? 0x20 : 0));
                vblank = false;
                w = false;
                return st;
            }
            case 4:  // OAMDATA
                return oam[oamaddr];
            case 7: {  // PPUDATA (buffered except for palette)
                uint16_t a2 = v & 0x3FFF;
                uint8_t ret;
                if (a2 >= 0x3F00) {
                    ret = ppu_mem_read(a2);
                    read_buf = ppu_mem_read(static_cast<uint16_t>(a2 - 0x1000));
                } else {
                    ret = read_buf;
                    read_buf = ppu_mem_read(a2);
                }
                ppu_data_bump(rendering_line_now());
                return ret;
            }
            default:
                return 0;  // write-only registers read as open bus (0 here)
        }
    }
    if (addr < 0x6000) {
        if (addr == 0x4015) return apu.read_status(cyc);
        if (addr == 0x4016) {
            uint8_t r;
            if (joy_strobe) {
                r = joy[0] & 1;
            } else {
                r = joy_shift[0] & 1;
                joy_shift[0] = static_cast<uint8_t>((joy_shift[0] >> 1) | 0x80);
            }
            return static_cast<uint8_t>(r | 0x40);
        }
        if (addr == 0x4017) return 0x40;
        return 0;
    }
    return sram[addr & 0x1FFF];  // $6000-$7FFF work RAM
}

void Machine::wr_io(uint16_t addr, uint8_t val) {
    if (addr < 0x4000) {
        ppu_sync();
        switch (addr & 7) {
            case 0: {  // PPUCTRL
                bool was_nmi = (ppuctrl & 0x80) != 0;
                ppuctrl = val;
                t = static_cast<uint16_t>((t & 0xF3FF) | ((val & 3) << 10));
                if ((val & 0x80) && !was_nmi && vblank) nmi_pending = true;
                break;
            }
            case 1:  // PPUMASK
                ppumask = val;
                break;
            case 3:  // OAMADDR
                oamaddr = val;
                break;
            case 4:  // OAMDATA
                oam[oamaddr++] = val;
                break;
            case 5:  // PPUSCROLL
                if (!w) {
                    fine_x = val & 7;
                    t = static_cast<uint16_t>((t & ~0x1F) | (val >> 3));
                    w = true;
                } else {
                    t = static_cast<uint16_t>((t & ~0x73E0) | ((val & 7) << 12) | ((val & 0xF8) << 2));
                    w = false;
                }
                break;
            case 6:  // PPUADDR
                if (!w) {
                    t = static_cast<uint16_t>((t & 0x00FF) | ((val & 0x3F) << 8));
                    w = true;
                } else {
                    t = static_cast<uint16_t>((t & 0x7F00) | val);
                    // While rendering, v takes t a few dots later, as on the hardware. Otherwise
                    // (vblank, rendering off) the CPU's next $2007 access must already see the new v.
                    if (rendering_enabled() && rendering_line_now()) {
                        pending_v_update = 3;
                    } else {
                        v = t;
                    }
                    w = false;
                }
                break;
            case 7:  // PPUDATA
                ppu_mem_write(v & 0x3FFF, val);
                ppu_data_bump(rendering_line_now());
                break;
            default:
                break;
        }
        return;
    }
    if (addr == 0x4014) {  // OAM DMA: 256 bytes from CPU page, 513/514 cycles
        uint16_t page = static_cast<uint16_t>(val << 8);
        for (int i = 0; i < 256; i++) oam[(oamaddr + i) & 0xFF] = rd(static_cast<uint16_t>(page + i));
        oam_dma_snapshot();
        cyc += 513 + (cyc & 1);
        return;
    }
    if (addr == 0x4016) {  // controller strobe
        joy_strobe = (val & 1) != 0;
        if (joy_strobe) joy_shift[0] = joy[0];
        return;
    }
    if (addr <= 0x4017) {  // APU registers ($4000-$4013, $4015, $4017)
        // The frame counter is shared by both voice sets; everything else goes to the owner's.
        if (addr == 0x4017 || snd_owner == kSndBoth) {
            apu.write(addr, val, cyc);
            sfx_apu.write(addr, val, cyc);
        } else if (snd_owner == kSndSfx) {
            sfx_apu.write(addr, val, cyc);
        } else {
            apu.write(addr, val, cyc);
        }
        return;
    }
    if (addr >= 0x6000 && addr < 0x8000) sram[addr & 0x1FFF] = val;
}

// ---------------------------------------------------------------- sound

void Machine::render_audio(int64_t until_cyc, std::vector<int16_t>& out) {
    const size_t start = out.size();
    apu.render(until_cyc, out);
    std::vector<int16_t> sfx;
    sfx_apu.render(until_cyc, sfx);
    // Both voice sets render the same clock span, so their sample counts match. The two outputs are
    // summed at full level, as the linear mixer defines: nothing is attenuated or compressed. The clamp
    // only stops 16-bit overflow when both sets peak together.
    const size_t n = std::min(out.size() - start, sfx.size());
    for (size_t i = 0; i < n; i++) {
        int32_t sum = static_cast<int32_t>(out[start + i]) + sfx[i];
        if (sum > 32767) sum = 32767;
        if (sum < -32768) sum = -32768;
        out[start + i] = static_cast<int16_t>(sum);
    }
}

void Machine::set_audio_rate_scale(double scale) {
    apu.set_rate_scale(scale);
    sfx_apu.set_rate_scale(scale);
}

void Machine::reset_sfx_apu() {
    // A level load must not carry sound across: with two voice sets, anything the sfx set
    // still held from the previous level (a channel the map engine left ringing, an
    // unfinished envelope, a looping DMC sample) has no music code rewriting its registers
    // here - the per-frame music writes all land on the music set - so it would keep
    // sounding in the new level as a background noise. Bring the set to power-on silence.
    sfx_apu.reset_silence(cyc);
    // The engine enables its channels once at startup (see reset()) and then relies on the
    // enables from the boot path; a note written before the engine's next $4015 write
    // reaches this set would otherwise be silent. Same constant as there.
    sfx_apu.write(0x4015, 0x0F, cyc);
}

// ---------------------------------------------------------------- MMC3

void Machine::wr_mapper(uint16_t addr, uint8_t val) {
    bool even = (addr & 1) == 0;
    switch (addr & 0xE000) {
        case 0x8000:
            if (even) {
                mmc3_sel = val & 7;
                prg_mode = (val >> 6) & 1;
                chr_inv = (val >> 7) & 1;
            } else {
                mmc3_r[mmc3_sel] = val;
            }
            update_mmc3_banks();
            break;
        case 0xA000:
            if (even) mirror = (val & 1) ? Mirror::Horizontal : Mirror::Vertical;
            // odd: PRG-RAM protect; SMB3 relies on always-enabled SRAM
            break;
        case 0xC000:
            if (even) {
                irq_latch = val;
            } else {
                irq_counter = 0;
                irq_reload = true;
            }
            break;
        case 0xE000:
            if (even) {
                irq_enable = false;
                irq_line = false;  // acknowledge
            } else {
                irq_enable = true;
            }
            break;
        default:
            break;
    }
}

void Machine::update_mmc3_banks() {
    const uint8_t prg_count = 32;  // 8 KB banks in the 256 KB PRG ROM
    uint8_t r6 = static_cast<uint8_t>(mmc3_r[6] & 0x3F);
    uint8_t r7 = static_cast<uint8_t>(mmc3_r[7] & 0x3F);
    if (prg_mode == 0) {
        prg_slot[0] = static_cast<uint8_t>(r6 % prg_count);
        prg_slot[2] = static_cast<uint8_t>(prg_count - 2);
    } else {
        prg_slot[0] = static_cast<uint8_t>(prg_count - 2);
        prg_slot[2] = static_cast<uint8_t>(r6 % prg_count);
    }
    prg_slot[1] = static_cast<uint8_t>(r7 % prg_count);
    prg_slot[3] = static_cast<uint8_t>(prg_count - 1);

    // CHR: two 2 KB banks (R0, R1) and four 1 KB banks (R2-R5); CHR A12 inversion swaps halves.
    uint8_t r0 = static_cast<uint8_t>(mmc3_r[0] & 0xFE);
    uint8_t r1 = static_cast<uint8_t>(mmc3_r[1] & 0xFE);
    uint8_t* c = chr_slot;
    if (chr_inv == 0) {
        c[0] = r0; c[1] = static_cast<uint8_t>(r0 + 1);
        c[2] = r1; c[3] = static_cast<uint8_t>(r1 + 1);
        c[4] = mmc3_r[2]; c[5] = mmc3_r[3]; c[6] = mmc3_r[4]; c[7] = mmc3_r[5];
    } else {
        c[4] = r0; c[5] = static_cast<uint8_t>(r0 + 1);
        c[6] = r1; c[7] = static_cast<uint8_t>(r1 + 1);
        c[0] = mmc3_r[2]; c[1] = mmc3_r[3]; c[2] = mmc3_r[4]; c[3] = mmc3_r[5];
    }
    // The 128 KB CHR-ROM has 128 1 KB banks; the chip ignores the high bank bits.
    for (int i = 0; i < 8; i++) c[i] &= 0x7F;
}

void Machine::mmc3_clock() {
    if (irq_counter == 0 || irq_reload) {
        irq_counter = irq_latch;
        irq_reload = false;
    } else {
        irq_counter--;
    }
    if (irq_counter == 0 && irq_enable) {
        irq_line = true;
        // The status bar is the bottom split of the frame, so the last IRQ wins: its rows start
        // on the next line. (Single-split games like SMB3 are unaffected.)
        if (ppu_line < kVisibleLines - 1) hud_top = ppu_line + 1;
    }
}

// ---------------------------------------------------------------- PPU memory

size_t Machine::nt_index(uint16_t addr) const {
    uint16_t nt = (addr >> 10) & 3;
    size_t phys = (mirror == Mirror::Vertical) ? (nt & 1) : (nt >> 1);
    return phys * 0x400 + (addr & 0x3FF);
}

uint8_t Machine::ppu_mem_read(uint16_t addr) {
    addr &= 0x3FFF;
    if (addr < 0x2000) return chr_read(addr);
    if (addr < 0x3F00) return vram[nt_index(addr)];
    return palram[pal_index(addr)];
}

void Machine::ppu_mem_write(uint16_t addr, uint8_t val) {
    addr &= 0x3FFF;
    if (addr < 0x2000) return;  // CHR is ROM on this board
    if (addr < 0x3F00) { vram[nt_index(addr)] = val; return; }
    palram[pal_index(addr)] = val;
}

// ---------------------------------------------------------------- dispatch

uint16_t Machine::dispatch(uint16_t addr) {
    if (addr < 0x8000) {
        std::fprintf(stderr, "dispatch to non-ROM address $%04X\n", addr);
        std::exit(2);
    }
    uint8_t bank = prg_slot[(addr >> 13) & 3];
    BankFn fn = g_bank_fns[bank];
    if (!fn) {
        std::fprintf(stderr, "no code in PRG bank %d (entry $%04X)\n", bank, addr);
        std::exit(2);
    }
    return fn(*this, addr);
}

uint16_t Machine::badEntry(uint16_t entry, int bank) {
    std::fprintf(stderr, "bank %d has no code at $%04X (window mapping is wrong)\n", bank, entry);
    std::exit(2);
}

void Machine::interrupt(uint16_t vector) {
    push16(pc);
    push(static_cast<uint8_t>((p | 0x20) & ~0x10));
    p |= 0x04;
    pc = rd16(vector);
    cyc += 7;
}

// ---------------------------------------------------------------- frame timing

void Machine::process_events() {
    // The MMC3 counter is clocked by A12 edges on the PPU's fetches (ppu_set_addr), which can happen
    // on any dot. So the PPU is run up to the CPU after every instruction. An IRQ raised by a fetch is
    // then taken after the instruction that is running, as on the 6502.
    ppu_sync();
    evt = cyc + 1;
}

// Drive the PPU address bus. The MMC3 counts rising edges of A12 (bit 12). Hardware only counts a
// rise after A12 has been low for about 3 CPU cycles (9 PPU dots). The short low gaps between sprite
// pattern fetches (4 dots) therefore do not clock the counter.
// Design informed by MesenCE (https://github.com/nesdev-org/MesenCE, GPLv3) NesPpu/MMC3: every
// pattern/nametable fetch drives the bus through one A12 watcher. Rewritten; no MesenCE code.
void Machine::ppu_set_addr(uint16_t addr) {
    ppu_addr_bus = addr;
    if (addr & 0x1000) {
        const bool rising = a12_low_dot >= 0 && ppu_dot_pos - a12_low_dot >= kA12FilterDots;
        a12_low_dot = -1;
        if (rising) mmc3_clock();
    } else if (a12_low_dot < 0) {
        a12_low_dot = ppu_dot_pos;
    }
}

bool Machine::rendering_line_now() const {
    return ppu_line < kVisibleLines || ppu_line == kPrerenderLine;
}

void Machine::ppu_sync() {
    const int64_t target = cyc * 3;
    while (ppu_dot_pos < target) ppu_step();
}

void Machine::ppu_step() {
    const int line = ppu_line;
    const int dot = ppu_dot;
    render_dot(line, dot);
    if (hud_layer_enabled && dot == 1 && line < kVisibleLines) hud_capture(line);
    if (line == kVblankLine && dot == 1) {
        // VRAM and the palette still hold this frame's values until the NMI handler runs.
        if (hud_layer_enabled) hud_compose();
        vblank = true;
        if (ppuctrl & 0x80) nmi_pending = true;
        frame_done = true;
    }
    if (line == kPrerenderLine && dot == 1) {
        hud_top = kNoHud;
        vblank = false;
        sprite0_hit = false;
        sprite_overflow = false;
    }
    ppu_dot_pos++;
    if (++ppu_dot == kDotsPerLine) {
        ppu_dot = 0;
        ppu_line = (ppu_line + 1) % kLinesPerFrame;
    }
}

void Machine::oam_dma_snapshot() {
    // The game renders the frame from `oam`, and it fills that buffer by DMAing the sprite
    // staging area, so freezing the Y records here makes them describe exactly the pieces of
    // the frame the game is about to show. Values come from the sliding window of writes
    // since the previous DMA: a drawer that skipped a piece leaves no record, and a slot
    // nobody wrote stays "not computed" instead of showing an old piece.
    for (int i = 0; i < 64; i++) {
        spr_y_frame[i] = (spr_y_seen >> i & 1) ? spr_y_rec[i] : kSprYNone;
    }
    spr_y_seen = 0;
    // Snapshot which entity owns which sprite region, and where it is: the game reassigns
    // regions every frame, so the wide pass cannot read that table afterwards and still be
    // talking about the frame in `oam`.
    for (int s = 0; s < 8; s++) {
        SprEntity& e = spr_ents[s];
        e.on = rd(smb3::Objects_State + s) != 0;
        e.base = static_cast<uint8_t>(rd(smb3::Object_SprRAM + s) & 0xFC);
        e.x = static_cast<uint16_t>(rd(smb3::Objects_X + s) | (rd(smb3::Objects_XHi + s) << 8));
        e.y = static_cast<uint16_t>(rd(smb3::Objects_Y + s) | (rd(smb3::Objects_YHi + s) << 8));
    }
    SprEntity& p = spr_ents[8];
    p.on = 1;
    p.base = static_cast<uint8_t>(rd(smb3::Player_SprOff) & 0xFC);
    p.x = static_cast<uint16_t>(rd(smb3::Player_X) | (rd(smb3::Player_XHi) << 8));
    p.y = static_cast<uint16_t>(rd(smb3::Player_Y) | (rd(smb3::Player_YHi) << 8));
}

void Machine::run_frame() {
    frame_done = false;
    while (!frame_done) {
        if (nmi_pending) {
            nmi_pending = false;
            interrupt(0xFFFA);
        } else if (irq_line && !(p & 0x04)) {
            interrupt(0xFFFE);
        }
        pc = dispatch(pc);
        while (cyc >= evt) process_events();
    }
}
