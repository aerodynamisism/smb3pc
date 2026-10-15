// 2A03 APU: two pulse channels, triangle, noise, DMC and the frame counter, mixed
// with the NES nonlinear mixer and passed through the console's output filters.
//
// Register writes arrive stamped with the CPU cycle they happened on. render()
// steps the APU one CPU cycle at a time, applies each write on its cycle, and
// emits 48 kHz samples through a band-limited resampler, so timing does not
// depend on when audio is drained.
//
// Written from the NESdev hardware description, not from another emulator's code.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

class Apu {
public:
    static constexpr int64_t kCpuHz = 1789773;
    static constexpr int64_t kSampleRate = 48000;

    Apu() {
        pulse_[0].ones_complement = true;
        rate_ = static_cast<double>(kSampleRate) / static_cast<double>(kCpuHz);
    }
    void reset();
    void write(uint16_t addr, uint8_t val, int64_t cyc);
    uint8_t read_status() const;
    // Advance to until_cyc, appending mono int16 samples to out.
    void render(int64_t until_cyc, std::vector<int16_t>& out);
    // Scale the output rate by scale (1.0 = nominal). Takes effect from the next cycle, without a
    // click. Used to keep the audio device queue at its target level.
    void set_rate_scale(double scale);

    // CPU memory read used by the DMC to fetch sample bytes. Set by the machine.
    std::function<uint8_t(uint16_t)> read_cpu;

private:
    struct Envelope {
        bool start = false;
        bool loop = false;      // also the length-counter halt flag
        bool constant = false;
        uint8_t period = 0;     // volume or envelope period (low 4 bits of reg 0)
        uint8_t div = 0;
        uint8_t decay = 0;
        void clock();
        uint8_t level() const { return constant ? period : decay; }
    };

    struct Pulse {
        bool enabled = false;
        Envelope env;
        uint8_t duty = 0;
        uint8_t seq = 0;
        uint16_t timer_period = 0;
        uint16_t timer = 0;
        uint8_t length = 0;
        bool sweep_enabled = false;
        bool sweep_negate = false;
        bool sweep_reload = false;
        uint8_t sweep_period = 0;
        uint8_t sweep_div = 0;
        uint8_t sweep_shift = 0;
        bool ones_complement = false;  // pulse 1 subtracts one extra when negating
        bool muted() const;
        uint16_t target_period() const;
        void sweep_clock();
        void timer_clock();
        uint8_t output() const;
    };

    struct Triangle {
        bool enabled = false;
        bool control = false;   // length halt flag
        uint8_t linear_reload = 0;
        uint8_t linear = 0;
        bool linear_flag = false;
        uint16_t timer_period = 0;
        uint16_t timer = 0;
        uint8_t length = 0;
        uint8_t seq = 0;
        void timer_clock();
        uint8_t output() const;
    };

    struct Noise {
        bool enabled = false;
        Envelope env;
        bool mode = false;
        uint16_t timer_period = 0;
        uint16_t timer = 0;
        uint8_t length = 0;
        uint16_t lfsr = 1;
        void timer_clock();
        uint8_t output() const;
    };

    // Delta modulation channel ($4010-$4013, $4015 bit 4). The IRQ output is not emulated.
    struct Dmc {
        bool loop = false;
        uint16_t period = 428;     // CPU cycles per output bit (NTSC rate table)
        uint16_t timer = 0;
        uint8_t level = 0;         // 7-bit output level
        uint16_t sample_addr = 0xC000;
        uint16_t sample_len = 1;
        uint16_t cur_addr = 0xC000;
        uint16_t bytes_left = 0;
        uint8_t buffer = 0;
        bool buffer_full = false;
        uint8_t shift = 0;
        uint8_t bits_left = 8;
        bool silence = true;
    };

    // First-order high-pass (90 Hz, 440 Hz) and low-pass (14 kHz) at the output rate.
    struct OutputFilter {
        float hp90_x = 0.0f, hp90_y = 0.0f;
        float hp440_x = 0.0f, hp440_y = 0.0f;
        float lp_y = 0.0f;
        float process(float x);
    };

    struct PendingWrite {
        int64_t cyc;
        uint16_t addr;
        uint8_t val;
    };

    void apply(uint16_t addr, uint8_t val);
    void step();
    void clock_frame();
    void quarter_frame();
    void half_frame();
    void clock_length(uint8_t& length, bool halt) const;
    void dmc_restart();
    void dmc_fill();
    void dmc_clock();
    float mix() const;

    Pulse pulse_[2];
    Triangle tri_;
    Noise noise_;
    Dmc dmc_;
    bool five_step_ = false;
    uint32_t frame_counter_ = 0;
    bool odd_cycle_ = false;
    int64_t t_ = 0;          // last CPU cycle stepped
    // Band-limited resampler: each change of the mixer adds a windowed-sinc step to buf_
    // at its output position; output n is the running sum of buf_ up to n.
    float prev_mix_ = 0.0f;
    double acc_ = 0.0;
    int64_t next_emit_ = 0;
    // Output position of CPU cycle t is pos_anchor_ + (t - t_anchor_) * rate_ (output samples).
    double pos_anchor_ = 0.0;
    int64_t t_anchor_ = 0;
    double rate_ = 0.0;
    float buf_[64] = {};
    OutputFilter filter_;
    std::deque<PendingWrite> pending_;
};
