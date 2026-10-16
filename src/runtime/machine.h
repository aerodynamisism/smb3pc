// Runtime for the translated SMB3 game code.
//
// The 6502 core state lives here, together with the NES memory map, the MMC3
// mapper, the PPU register file and the frame timing. Generated bank functions
// (build/gen/bank_NN.cpp) operate on a Machine through the helpers below.
#pragma once

#include <cstdint>
#include <cstddef>

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

    // ---- status-bar layer ----
    // The rows from the split IRQ down are composed at the end of the frame from the scroll,
    // palette and bank state captured on the first status-bar line, so the status bar does not
    // depend on where the CPU was when each line was drawn.
    static constexpr int kNoHud = 240;
    int hud_top = kNoHud;               // first status-bar row of this frame (last split IRQ wins)
    bool hud_layer_enabled = true;      // false draws every row from the per-dot pipeline
    // State each visible line is drawn from, captured at its dot 1. The game's split writes land
    // over several status-bar lines, so one snapshot per frame would not match the per-dot output.
    struct HudLine {
        uint16_t v = 0;
        uint8_t fine_x = 0, ctrl = 0, mask = 0;
        bool on = false;                // rendering was enabled
        uint8_t palram[32] = {};
        uint8_t chr_slot[8] = {};
        uint8_t oam[256] = {};          // sprite RAM: sprites are absolute-positioned, but the
                                        // per-dot pass mixes them with the (then wrong) playfield
                                        // background, so the compositor re-evaluates them here
    };
    HudLine hud_lines[240] = {};
    void hud_capture(int line);
    void hud_compose();

    // ---- input ----
    uint8_t joy[2] = {0, 0};        // A B Select Start Up Down Left Right (bit 0 = A)
    uint8_t joy_shift[2] = {0, 0};
    bool joy_strobe = false;

    Apu apu;

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
