#include "runtime/machine.h"

#include <algorithm>
#include <climits>
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
    if (sandbox && addr < 0x6000) return 0;  // a drawer is being re-run: no side effects
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
    if (sandbox) return;  // a drawer is being re-run: no PPU/APU/DMA side effects
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
        // The sprite staging area is $0200; a plant held over a skipped frame is written there first.
        if (val == 0x02) sprite_hold_before_dma();
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
        // The frame counter ($4017) is hardware-global. The enables register ($4015) is routed by
        // owner, but a write that clears every channel is a "stop everything" gesture: the map's
        // level-entry halt writes $4015=$00 from the sfx side, and if it did not reach both sets
        // the music set would keep its last note ringing through the whole transition. (The pause
        // routine's $4015 writes get the same treatment in translate.py.) The $0F that usually
        // follows stays per-owner.
        if (addr == 0x4017 || snd_owner == kSndBoth ||
            (addr == 0x4015 && val == 0x00 && snd_owner == kSndSfx)) {
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
        frame_scroll_set = false;  // the next frame's camera is captured on its first line
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

// Re-run one object's drawer so that it draws every piece, and keep the pieces for the wide pass.
// Called from the object loop right after the object's own state action (see translate.py's
// WIDE_CALLS), where X is the slot and the mapper holds that object's banks - the two things a
// later re-run cannot reconstruct. The position to re-run from comes from the OAM DMA snapshot
// (spr_ents) rather than from RAM: the state action draws first and moves afterwards, so RAM
// already holds where the object will be next frame, and a piece the drawer places against the
// level instead of against the object (a piranha plant's pipe mask sits on the pipe's own row)
// would be measured from a position the frame never drew with - the mask came back one row low
// and the plant's head showed through the pipe's rim. The state action starts by recomputing the
// object's horizontal and vertical visibility from where it sits on screen
// (Object_DetermineVertVis/HorzVis), so clearing those bits is not enough: the re-run is given the
// scrolls that put the object at 100,100, where no piece of it is off-screen and nothing is culled.
// That matters because a drawer skips a piece by skipping its Y-byte store, and some drawers then
// read the missing value back (a troopa's head is drawn only when the body under it was), so a
// partly culled draw would hand back a stale mix. It also makes the result independent of the
// frame's edge: an object half past it keeps every piece on the frames where it sits a pixel
// further out, instead of losing exactly those pieces - which is what made sprites flicker at
// the picture's edge.
//
// The re-run draws into a scratch sprite region rather than the one the game used for this object:
// the region rotates every frame and a drawer may shift its own copy first (a piranha plant adds 8
// so two sprites fit in front of it), which is how a sprite ends up past OAM's end and disappears
// for a frame. Only the pieces' contents and their order matter, and the order is kept.
//
// Everything the re-run touches is put back afterwards: RAM (including the staging area and the
// audio routing table), the registers, the cycle and event counters, the mapper and the frame's
// sprite records. The game never sees the second run; only wide_pieces[] survives it.
void Machine::wide_after_state_action() {
    const int slot = x & 7;
    wide_piece_count[slot] = 0;
    wide_piece_valid[slot] = false;
    if (!wide_enabled || sandbox) return;
    if (rd(smb3::World_EnterState) != 3) return;  // only inside a level
    if (rd(static_cast<uint16_t>(smb3::Objects_State + slot)) == 0) return;  // dead or empty

    // The position the frame's own draw used, snapshotted with the frame in `oam` (see above). A
    // slot the snapshot does not have alive was not drawn this frame at all, and the wide pass
    // skips it for the same reason, so there is nothing to keep.
    const SprEntity& snap = spr_ents[slot];
    if (!snap.on) return;

    const int cam = ram[smb3::Horz_Scroll & 0x7FF] | (ram[smb3::Horz_Scroll_Hi & 0x7FF] << 8);
    const int obj_x = snap.x;
    const int obj_y = snap.y;
    const int sx = obj_x - cam;
    // Wholly inside the pasted frame (the widest object is kWideBox pixels): the frame already
    // shows it and the wide pass never asks for its pieces. The two pixels of margin absorb the
    // difference between the live scroll read here and the frame's captured scroll, so an object
    // the wide pass wants pieces for was always re-run a frame or two early.
    const int inside_lo = kWideCrop + 2, inside_hi = 256 - kWideCrop - 2;
    if (sx >= inside_lo && sx + kWideBox <= inside_hi) return;
    // Outside the wide picture itself (the frame reaches kWideExtra pixels to either side).
    if (sx + kWideBox <= -kWideExtra || sx >= kWideW - kWideExtra) return;

    struct Saved {
        uint8_t a, x, y, s, p;
        uint16_t pc;
        int64_t cyc, evt;
        uint8_t snd_owner;
        bool sandbox;
        uint8_t prg_slot[4], chr_slot[8];
        uint8_t mmc3_sel, mmc3_r[8], prg_mode, chr_inv;
        bool irq_enable, irq_reload, irq_line;
        uint8_t irq_latch, irq_counter;
        uint8_t spr_y_rec[64], spr_y_frame[64];
        uint64_t spr_y_seen;
        uint8_t ram[0x800];
        uint8_t snd_sav[256];
    } sv;
    sv.a = a; sv.x = x; sv.y = y; sv.s = s; sv.p = p; sv.pc = pc;
    sv.cyc = cyc; sv.evt = evt; sv.snd_owner = snd_owner; sv.sandbox = sandbox;
    std::memcpy(sv.prg_slot, prg_slot, 4);
    std::memcpy(sv.chr_slot, chr_slot, 8);
    sv.mmc3_sel = mmc3_sel;
    std::memcpy(sv.mmc3_r, mmc3_r, 8);
    sv.prg_mode = prg_mode; sv.chr_inv = chr_inv;
    sv.irq_enable = irq_enable; sv.irq_reload = irq_reload; sv.irq_line = irq_line;
    sv.irq_latch = irq_latch; sv.irq_counter = irq_counter;
    std::memcpy(sv.spr_y_rec, spr_y_rec, sizeof spr_y_rec);
    std::memcpy(sv.spr_y_frame, spr_y_frame, sizeof spr_y_frame);
    sv.spr_y_seen = spr_y_seen;
    std::memcpy(sv.ram, ram, sizeof ram);
    std::memcpy(sv.snd_sav, snd_sav, sizeof snd_sav);

    // The second draw writes into a scratch sprite region instead of the one the game used. The
    // game's region rotates every frame and a drawer may shift its own copy first (a piranha
    // plant adds 8 to its own SprRAM so two sprites fit in front of it), which is exactly how a
    // sprite ends up past OAM's end and disappears for a frame. Only the pieces' contents and
    // their order matter here, so the drawer gets a window with room for everything it writes:
    // eight bytes before its region (a troopa's head) and the flight of feet after its sprites.
    const int base = ram[(smb3::Object_SprRAM + slot) & 0x7FF] & 0xFC;
    constexpr int kScratch = 0x80;
    constexpr int kScratchLo = kScratch - 48;
    constexpr int kScratchHi = kScratch + 64;
    ram[(smb3::Object_SprRAM + slot) & 0x7FF] = kScratch;
    for (int off = kScratchLo; off < kScratchHi; off++) ram[0x200 + off] = 0xF8;

    ram[(smb3::Objects_SprHVis + slot) & 0x7FF] = 0;
    ram[(smb3::Objects_SprVVis + slot) & 0x7FF] = 0;
    // Rewind the object to the position its own draw used (see the note above): the state action
    // has moved it since, and the pieces are placed against the snapshotted position.
    ram[(smb3::Objects_X + slot) & 0x7FF] = static_cast<uint8_t>(obj_x);
    ram[(smb3::Objects_XHi + slot) & 0x7FF] = static_cast<uint8_t>(obj_x >> 8);
    ram[(smb3::Objects_Y + slot) & 0x7FF] = static_cast<uint8_t>(obj_y);
    ram[(smb3::Objects_YHi + slot) & 0x7FF] = static_cast<uint8_t>(obj_y >> 8);
    // Put the object in the middle of the screen for the re-run, both ways. The state action starts
    // with Object_DetermineVertVis / Object_DetermineHorzVis (PRG000.asm:2071), which recompute the
    // visibility bits from the object's screen position, so clearing them is not enough: an object
    // half past the frame's edge would still lose exactly the pieces the wide picture needs, and it
    // would lose them only on the frames where it sits a pixel further out - that is the sprite
    // flicker at the frame's edge. With the object at 100,100 every piece it draws is on-screen.
    const int draw_cam = obj_x - 100;
    const int draw_vscroll = obj_y - 100;
    ram[smb3::Horz_Scroll & 0x7FF] = static_cast<uint8_t>(draw_cam & 0xFF);
    ram[smb3::Horz_Scroll_Hi & 0x7FF] = static_cast<uint8_t>((draw_cam >> 8) & 0xFF);
    ram[smb3::Level_VertScroll & 0x7FF] = static_cast<uint8_t>(draw_vscroll & 0xFF);
    ram[smb3::Level_VertScrollH & 0x7FF] = static_cast<uint8_t>((draw_vscroll >> 8) & 0xFF);
    ram[smb3::Vert_Scroll & 0x7FF] = static_cast<uint8_t>(draw_vscroll & 0xFF);

    constexpr uint16_t kSentinel = 0x8000;  // where the sandboxed call "returns"
    sandbox = true;
    x = static_cast<uint8_t>(slot);
    a = 0;
    p = 0x24;
    s = static_cast<uint8_t>(sv.s - 2);
    push16(static_cast<uint16_t>(kSentinel - 1));  // RTS lands on the sentinel
    evt = INT64_MAX;  // the generated code re-dispatches forever if an event is "due"
    uint16_t at = smb3::Object_DoStateAction;
    for (int steps = 0; steps < 20000; steps++) {
        if (at == kSentinel) break;
        at = dispatch(at);
    }

    // Reduce the pieces to a position relative to the object, measured with the camera and
    // vertical scroll the drawer itself used: the wide pass places them against the frame, whose
    // scroll may be a few pixels different (the game advances Horz_Scroll before it draws).
    int n = 0;
    // Relative to where the drawer itself put the object (the re-run's shifted screen).
    const int obj_sx = obj_x - draw_cam, obj_sy = obj_y - draw_vscroll;
    for (int off = kScratchLo; off < kScratchHi && n < kWidePieces; off += 4) {
        const uint8_t ry = ram[0x200 + off];
        if (ry == 0xF8 || ry >= 239) continue;
        const int8_t dx = static_cast<int8_t>(static_cast<uint8_t>(ram[0x200 + off + 3] - obj_sx));
        const int8_t dy = static_cast<int8_t>(static_cast<uint8_t>(ry - obj_sy));
        // A drawer's pieces sit within 64 pixels of its own origin; anything farther is a stale
        // byte from another writer of the scratch window.
        if (dx < -64 || dx > 64 || dy < -64 || dy > 64) continue;
        // Keep the order the pieces would have in the game's own sprite rotation, so sprites of
        // different objects still stack as the hardware would stack them. The offset from the
        // scratch region back to the drawer's real region is signed: a piece written before the
        // region (a troopa's head at -8, a piranha's pipe mask at -8/-4) has to stay ahead of the
        // pieces written after it, and folding it to 0 would tie it with the first piece at the
        // region's start, leaving the two in an arbitrary paint order that changes from frame to
        // frame - which is what made a plant's pipe mask blink off its body.
        const int order = base + off - kScratch;
        wide_pieces[slot][n] = WidePiece{dx, dy, ram[0x200 + off + 1], ram[0x200 + off + 2], order};
        n++;
    }
    // A plant's skipped frame: its drawer wrote nothing, so the pieces it drew on the last frame are
    // held (sprite_hold_before_dma writes the same ones into the frame), and the wide pass uses those.
    if (n == 0 && held_valid[slot] && plant_skip_phase(slot)) {
        for (int k = 0; k < held_count[slot]; k++) {
            WidePiece w = held_pieces[slot][k];
            w.slot = base + w.slot;  // the region this frame's drawer uses (see sprite_hold_before_dma)
            wide_pieces[slot][k] = w;
        }
        n = held_count[slot];
    }
    wide_piece_count[slot] = static_cast<uint8_t>(n);
    wide_piece_valid[slot] = true;

    a = sv.a; x = sv.x; y = sv.y; s = sv.s; p = sv.p; pc = sv.pc;
    cyc = sv.cyc; evt = sv.evt; snd_owner = sv.snd_owner; sandbox = sv.sandbox;
    std::memcpy(prg_slot, sv.prg_slot, 4);
    std::memcpy(chr_slot, sv.chr_slot, 8);
    mmc3_sel = sv.mmc3_sel;
    std::memcpy(mmc3_r, sv.mmc3_r, 8);
    prg_mode = sv.prg_mode; chr_inv = sv.chr_inv;
    irq_enable = sv.irq_enable; irq_reload = sv.irq_reload; irq_line = sv.irq_line;
    irq_latch = sv.irq_latch; irq_counter = sv.irq_counter;
    std::memcpy(spr_y_rec, sv.spr_y_rec, sizeof spr_y_rec);
    std::memcpy(spr_y_frame, sv.spr_y_frame, sizeof spr_y_frame);
    spr_y_seen = sv.spr_y_seen;
    std::memcpy(ram, sv.ram, sizeof ram);
    std::memcpy(snd_sav, sv.snd_sav, sizeof snd_sav);
}

// Piranha plants hold their last drawn pieces over the frame their drawer skips (see machine.h). Runs
// once per frame, just before the staging buffer is DMAed to OAM. A plant whose region was written this
// frame has its pieces recorded, relative to its own screen position, so they can be replayed on the
// frame that skips. On a skipped frame (no sprite-Y store into its region) the recorded pieces are
// written back, and their Y stores are reported to the sprite records, so the widescreen pass treats
// them like any drawn piece. A held frame is not followed by another, so a plant never freezes.
bool Machine::plant_skip_phase(int s) const {
    const uint8_t flip = ram[smb3::Objects_FlipBits + s];
    return ((ram[smb3::Objects_Var4 + s] + ((flip & 0x80) ? 2 : 0)) & 3) == 0;
}

void Machine::sprite_hold_before_dma() {
    // The scroll the game's drawers used this frame, as the object positions are made relative to it.
    const int cam = ram[smb3::Horz_Scroll & 0x7FF] | (ram[smb3::Horz_Scroll_Hi & 0x7FF] << 8);
    const int vscroll = ram[smb3::Level_VertScroll & 0x7FF] | (ram[smb3::Level_VertScrollH & 0x7FF] << 8);
    for (int s = 0; s < 8; s++) {
        const uint8_t id = ram[smb3::Level_ObjectID + s];
        const bool plant = id >= 0xA0 && id <= 0xA7 && ram[smb3::Objects_State + s] != 0;
        if (!plant) {
            held_valid[s] = false;
            continue;
        }
        if (held_id[s] != id) {
            held_valid[s] = false;
            held_id[s] = id;
        }
        const int base = ram[smb3::Object_SprRAM + s] & 0xFC;
        // The mask sprites sit 8 and 4 bytes before the region, the plant's own sprites after it. The
        // sprite index is a byte, so a region near the top of OAM wraps to its bottom, as the game's own
        // writes do; every offset below is taken modulo 256.
        const int obj_sx = (ram[smb3::Objects_XHi + s] << 8 | ram[smb3::Objects_X + s]) - cam;
        const int obj_sy = (ram[smb3::Objects_YHi + s] << 8 | ram[smb3::Objects_Y + s]) - vscroll;
        if (!plant_skip_phase(s)) {
            // A drawn frame: keep its pieces, relative to the region, so a later skipped frame can place them.
            // A drawn frame with no pieces (the plant is past the frame's edge) keeps the last pieces it had,
            // so a skipped frame that brings the plant back inside the picture still draws them.
            if (wide_enabled && wide_piece_valid[s] && wide_piece_count[s] > 0) {
                // The widescreen re-run already has every piece the drawer computed, visible or not.
                held_count[s] = wide_piece_count[s];
                for (int k = 0; k < held_count[s]; k++) {
                    held_pieces[s][k] = wide_pieces[s][k];
                    held_pieces[s][k].slot = wide_pieces[s][k].slot - base;
                }
            } else {
                int n = 0;
                for (int rel = -8; rel < 24 && n < kWidePieces; rel += 4) {
                    const int off = (base + rel) & 0xFF;
                    const uint8_t ry = ram[0x200 + off];
                    if (ry >= 239) continue;  // hidden or below the picture
                    const int8_t dx = static_cast<int8_t>(static_cast<uint8_t>(ram[0x200 + off + 3] - obj_sx));
                    const int8_t dy = static_cast<int8_t>(static_cast<uint8_t>(ry - obj_sy));
                    if (dx < -64 || dx > 64 || dy < -64 || dy > 64) continue;
                    held_pieces[s][n++] = WidePiece{dx, dy, ram[0x200 + off + 1], ram[0x200 + off + 2], rel};
                }
                if (n > 0) held_count[s] = static_cast<uint8_t>(n);
            }
            held_valid[s] = true;
        } else if (held_valid[s]) {
            // Skipped frame: the game drew nothing for the plant, so write its last drawn pieces into this frame's region.
            for (int k = 0; k < held_count[s]; k++) {
                const WidePiece& w = held_pieces[s][k];
                const int off = (base + w.slot) & 0xFF;
                // Only pieces on the 256-pixel frame: the game never writes a piece that lies outside it,
                // and a wrapped X would show it on the wrong side.
                const int px = obj_sx + w.dx, py = obj_sy + w.dy;
                if (px < 0 || px > 248 || py < 0 || py >= 239) continue;
                const uint16_t ea = static_cast<uint16_t>(0x200 + off);
                ram[ea] = static_cast<uint8_t>(py);
                ram[ea + 1] = w.tile;
                ram[ea + 2] = w.attr;
                ram[ea + 3] = static_cast<uint8_t>(px);
                spr_y_record(ea, ram[ea]);
            }
        }
    }
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
