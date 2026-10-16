// 2A03 APU wrapper around Blargg Nes_Snd_Emu 0.1.7 (LGPL, see nes_apu/LGPL.txt).
//
// Two mixer modes. Linear (default): a single Blip_Buffer receives all five
// oscillators with the linear per-oscillator gains from Nes_Apu::volume().
// Nonlinear: each oscillator renders into its own Blip_Buffer and the five
// channel levels are combined with the hardware mixer curves, so loud
// passages compress the way the 2A03 output stage does. enable_nonlinear()
// and Nonlinear_Buffer are never used (the latter is not vendored); the
// nonlinear combine below is written against the same curves.
//
// Register writes arrive stamped with the absolute CPU cycle. They are queued
// and forwarded to Nes_Apu with frame-relative times during render() /
// read_status(), so timing does not depend on when audio is drained.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

enum class Mixing {
    Linear,     // linear per-oscillator gains (default)
    Nonlinear,  // hardware mixer curves, hardware-relative levels
};

class Apu {
public:
    static constexpr int64_t kCpuHz = 1789773;
    static constexpr int64_t kSampleRate = 48000;

    Apu();
    ~Apu();
    Apu(const Apu&) = delete;
    Apu& operator=(const Apu&) = delete;

    void reset();
    void write(uint16_t addr, uint8_t val, int64_t cyc);
    // Cycle-stamped like write(): runs the APU forward to cyc before reading.
    uint8_t read_status(int64_t cyc);
    // Mixer mode. Linear is the default. Switching re-routes the oscillators
    // and clears the buffers; call between frames (never mid-render).
    void set_mixing(Mixing mode);
    Mixing mixing() const;
    // Advance to until_cyc, appending mono int16 samples to out.
    void render(int64_t until_cyc, std::vector<int16_t>& out);
    // Scale the output rate by scale (1.0 = nominal). Used to keep the audio
    // device queue at its target level.
    void set_rate_scale(double scale);

    // CPU memory read used by the DMC to fetch sample bytes. Set by the machine.
    std::function<uint8_t(uint16_t)> read_cpu;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    static void nonlinear_mix(Impl& d, std::vector<int16_t>& out);
};
