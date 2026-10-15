#include "runtime/apu.h"

#include <algorithm>
#include <cmath>

namespace {

// Length counter lookup, indexed by the 5-bit value written to $4003/$400B/$400F.
constexpr uint8_t kLengthTable[32] = {10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
                                      12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};

// Pulse duty cycles (12.5%, 25%, 50%, 75%).
constexpr uint8_t kDuty[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1},
};

// NTSC noise period table.
constexpr uint16_t kNoisePeriod[16] = {4,   8,   16,  32,  64,  96,  128, 160,
                                       202, 254, 380, 508, 762, 1016, 2034, 4068};

// Triangle sequence: 15 down to 0, then 0 up to 15.
constexpr uint8_t kTriangleSeq[32] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
                                      0,  1,  2,  3,  4,  5,  6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

// Band-limited step kernel for the resampler. Row p is the impulse response of a windowed-sinc
// low-pass (cutoff 20 kHz at 48 kHz) for an impulse at fractional position p / kPhases between
// two output samples. Each row sums to 1, so a mixer change of dx adds exactly dx to the output.
constexpr int kPhases = 64;
constexpr int kTaps = 32;
constexpr int kHalfTaps = kTaps / 2;
constexpr int64_t kBufMask = 63;  // resampler ring buffer (must cover kTaps + the settle lag)
constexpr double kPi = 3.14159265358979323846;

struct StepKernel {
    float h[kPhases][kTaps];
    StepKernel() {
        const double wc = 2.0 * kPi * 20000.0 / static_cast<double>(Apu::kSampleRate);
        for (int p = 0; p < kPhases; p++) {
            const double phi = static_cast<double>(p) / kPhases;
            double row[kTaps];
            double sum = 0.0;
            for (int k = 0; k < kTaps; k++) {
                const double d = (k - kHalfTaps) - phi;
                const double ad = std::fabs(d);
                const double w = ad < kHalfTaps ? 0.42 + 0.5 * std::cos(kPi * d / kHalfTaps) +
                                                      0.08 * std::cos(2.0 * kPi * d / kHalfTaps)
                                                : 0.0;
                const double x = wc * d;
                const double s = std::fabs(x) < 1e-12 ? 1.0 : std::sin(x) / x;
                row[k] = wc / kPi * s * w;
                sum += row[k];
            }
            for (int k = 0; k < kTaps; k++) h[p][k] = static_cast<float>(row[k] / sum);
        }
    }
};

const StepKernel& step_kernel() {
    static const StepKernel kernel;
    return kernel;
}

}  // namespace

void Apu::reset() {
    auto reader = read_cpu;  // keep the memory hook across a reset
    *this = Apu();
    read_cpu = reader;
}

// ---------------------------------------------------------------- register writes

void Apu::write(uint16_t addr, uint8_t val, int64_t cyc) {
    pending_.push_back(PendingWrite{cyc, addr, val});
}

uint8_t Apu::read_status() const {
    // Length-counter status for the four length-counted channels, and DMC bytes remaining.
    return static_cast<uint8_t>((pulse_[0].length > 0 ? 0x01 : 0) | (pulse_[1].length > 0 ? 0x02 : 0) |
                                (tri_.length > 0 ? 0x04 : 0) | (noise_.length > 0 ? 0x08 : 0) |
                                (dmc_.bytes_left > 0 ? 0x10 : 0));
}

void Apu::apply(uint16_t addr, uint8_t val) {
    switch (addr) {
        // Pulse 1 ($4000-$4003) and pulse 2 ($4004-$4007).
        case 0x4000:
        case 0x4004: {
            Pulse& p = pulse_[(addr - 0x4000) >> 2];
            p.duty = (val >> 6) & 3;
            p.env.loop = (val & 0x20) != 0;
            p.env.constant = (val & 0x10) != 0;
            p.env.period = val & 0x0F;
            break;
        }
        case 0x4001:
        case 0x4005: {
            Pulse& p = pulse_[(addr - 0x4000) >> 2];
            p.sweep_enabled = (val & 0x80) != 0;
            p.sweep_period = (val >> 4) & 7;
            p.sweep_negate = (val & 0x08) != 0;
            p.sweep_shift = val & 7;
            p.sweep_reload = true;
            break;
        }
        case 0x4002:
        case 0x4006: {
            Pulse& p = pulse_[(addr - 0x4000) >> 2];
            p.timer_period = static_cast<uint16_t>((p.timer_period & 0x700) | val);
            break;
        }
        case 0x4003:
        case 0x4007: {
            Pulse& p = pulse_[(addr - 0x4000) >> 2];
            p.timer_period = static_cast<uint16_t>((p.timer_period & 0x0FF) | ((val & 7) << 8));
            if (p.enabled) p.length = kLengthTable[val >> 3];
            p.seq = 0;
            p.env.start = true;
            break;
        }
        // Triangle ($4008-$400B).
        case 0x4008:
            tri_.control = (val & 0x80) != 0;
            tri_.linear_reload = val & 0x7F;
            break;
        case 0x400A:
            tri_.timer_period = static_cast<uint16_t>((tri_.timer_period & 0x700) | val);
            break;
        case 0x400B:
            tri_.timer_period = static_cast<uint16_t>((tri_.timer_period & 0x0FF) | ((val & 7) << 8));
            if (tri_.enabled) tri_.length = kLengthTable[val >> 3];
            tri_.linear_flag = true;
            break;
        // Noise ($400C-$400F).
        case 0x400C:
            noise_.env.loop = (val & 0x20) != 0;
            noise_.env.constant = (val & 0x10) != 0;
            noise_.env.period = val & 0x0F;
            break;
        case 0x400E:
            noise_.mode = (val & 0x80) != 0;
            noise_.timer_period = kNoisePeriod[val & 0x0F];
            break;
        case 0x400F:
            if (noise_.enabled) noise_.length = kLengthTable[val >> 3];
            noise_.env.start = true;
            break;
        // DMC ($4010-$4013).
        case 0x4010: {
            static constexpr uint16_t kRate[16] = {428, 380, 340, 320, 286, 254, 226, 214,
                                                   190, 160, 142, 128, 106, 84, 72, 54};
            dmc_.loop = (val & 0x40) != 0;
            dmc_.period = kRate[val & 0x0F];
            break;
        }
        case 0x4011:
            dmc_.level = val & 0x7F;
            break;
        case 0x4012:
            dmc_.sample_addr = static_cast<uint16_t>(0xC000 + val * 64);
            break;
        case 0x4013:
            dmc_.sample_len = static_cast<uint16_t>(val * 16 + 1);
            break;
        // Status and frame counter.
        case 0x4015:
            pulse_[0].enabled = (val & 0x01) != 0;
            pulse_[1].enabled = (val & 0x02) != 0;
            tri_.enabled = (val & 0x04) != 0;
            noise_.enabled = (val & 0x08) != 0;
            if (!pulse_[0].enabled) pulse_[0].length = 0;
            if (!pulse_[1].enabled) pulse_[1].length = 0;
            if (!tri_.enabled) tri_.length = 0;
            if (!noise_.enabled) noise_.length = 0;
            if (!(val & 0x10)) {
                dmc_.bytes_left = 0;
            } else if (dmc_.bytes_left == 0) {
                dmc_restart();
            }
            break;
        case 0x4017:
            five_step_ = (val & 0x80) != 0;
            frame_counter_ = 0;
            break;
        default:
            break;  // other unused registers
    }
}

// ---------------------------------------------------------------- DMC

void Apu::dmc_restart() {
    dmc_.cur_addr = dmc_.sample_addr;
    dmc_.bytes_left = dmc_.sample_len;
    dmc_fill();
}

void Apu::dmc_fill() {
    // Fetch the next sample byte when the buffer is empty. The CPU stall this costs is not modelled.
    if (dmc_.buffer_full || dmc_.bytes_left == 0 || !read_cpu) return;
    dmc_.buffer = read_cpu(dmc_.cur_addr);
    dmc_.buffer_full = true;
    dmc_.cur_addr = dmc_.cur_addr == 0xFFFF ? 0x8000 : static_cast<uint16_t>(dmc_.cur_addr + 1);
    dmc_.bytes_left--;
    if (dmc_.bytes_left == 0 && dmc_.loop) dmc_restart();
}

void Apu::dmc_clock() {
    if (dmc_.timer > 0) {
        dmc_.timer--;
        return;
    }
    dmc_.timer = dmc_.period;
    if (!dmc_.silence) {
        // Each bit steps the level by +2 or -2, clamped to 0..127.
        if (dmc_.shift & 1) {
            if (dmc_.level <= 125) dmc_.level = static_cast<uint8_t>(dmc_.level + 2);
        } else if (dmc_.level >= 2) {
            dmc_.level = static_cast<uint8_t>(dmc_.level - 2);
        }
    }
    dmc_.shift = static_cast<uint8_t>(dmc_.shift >> 1);
    if (--dmc_.bits_left == 0) {
        dmc_.bits_left = 8;
        if (dmc_.buffer_full) {
            dmc_.silence = false;
            dmc_.shift = dmc_.buffer;
            dmc_.buffer_full = false;
        } else {
            dmc_.silence = true;
        }
        dmc_fill();
    }
}

// ---------------------------------------------------------------- units

void Apu::Envelope::clock() {
    if (start) {
        start = false;
        decay = 15;
        div = period;
    } else if (div == 0) {
        div = period;
        if (decay > 0) {
            decay--;
        } else if (loop) {
            decay = 15;
        }
    } else {
        div--;
    }
}

bool Apu::Pulse::muted() const {
    return timer_period < 8 || target_period() > 0x7FF;
}

uint16_t Apu::Pulse::target_period() const {
    uint16_t change = static_cast<uint16_t>(timer_period >> sweep_shift);
    if (sweep_negate) {
        uint16_t sub = static_cast<uint16_t>(change + (ones_complement ? 1 : 0));
        return static_cast<uint16_t>(timer_period > sub ? timer_period - sub : 0);
    }
    return static_cast<uint16_t>(timer_period + change);
}

void Apu::Pulse::sweep_clock() {
    if (sweep_div == 0 && sweep_enabled && sweep_shift > 0 && !muted()) {
        timer_period = target_period();
    }
    if (sweep_div == 0 || sweep_reload) {
        sweep_div = sweep_period;
        sweep_reload = false;
    } else {
        sweep_div--;
    }
}

void Apu::Pulse::timer_clock() {
    if (timer == 0) {
        timer = timer_period;
        seq = static_cast<uint8_t>((seq + 7) & 7);  // count downwards
    } else {
        timer--;
    }
}

uint8_t Apu::Pulse::output() const {
    if (!enabled || length == 0 || muted()) return 0;
    if (!kDuty[duty][seq]) return 0;
    return env.level();
}

void Apu::Triangle::timer_clock() {
    if (timer == 0) {
        timer = timer_period;
        if (length > 0 && linear > 0) seq = static_cast<uint8_t>((seq + 1) & 31);
    } else {
        timer--;
    }
}

uint8_t Apu::Triangle::output() const {
    if (!enabled) return 0;
    return kTriangleSeq[seq];
}

void Apu::Noise::timer_clock() {
    if (timer == 0) {
        timer = timer_period;
        uint16_t other = mode ? (lfsr >> 6) & 1 : (lfsr >> 1) & 1;
        uint16_t feedback = (lfsr & 1) ^ other;
        lfsr = static_cast<uint16_t>((lfsr >> 1) | (feedback << 14));
    } else {
        timer--;
    }
}

uint8_t Apu::Noise::output() const {
    if (!enabled || length == 0 || (lfsr & 1)) return 0;
    return env.level();
}

// ---------------------------------------------------------------- frame counter

void Apu::clock_length(uint8_t& length, bool halt) const {
    if (length > 0 && !halt) length--;
}

void Apu::quarter_frame() {
    pulse_[0].env.clock();
    pulse_[1].env.clock();
    noise_.env.clock();
    if (tri_.linear_flag) {
        tri_.linear = tri_.linear_reload;
    } else if (tri_.linear > 0) {
        tri_.linear--;
    }
    if (!tri_.control) tri_.linear_flag = false;
}

void Apu::half_frame() {
    clock_length(pulse_[0].length, pulse_[0].env.loop);
    clock_length(pulse_[1].length, pulse_[1].env.loop);
    clock_length(tri_.length, tri_.control);
    clock_length(noise_.length, noise_.env.loop);
    pulse_[0].sweep_clock();
    pulse_[1].sweep_clock();
}

void Apu::clock_frame() {
    frame_counter_++;
    if (!five_step_) {
        switch (frame_counter_) {
            case 7457: quarter_frame(); break;
            case 14913: quarter_frame(); half_frame(); break;
            case 22371: quarter_frame(); break;
            case 29829: quarter_frame(); half_frame(); frame_counter_ = 0; break;
            default: break;
        }
    } else {
        switch (frame_counter_) {
            case 7457: quarter_frame(); break;
            case 14913: quarter_frame(); half_frame(); break;
            case 22371: quarter_frame(); break;
            case 37281: quarter_frame(); half_frame(); frame_counter_ = 0; break;
            default: break;
        }
    }
}

// ---------------------------------------------------------------- synthesis

void Apu::step() {
    clock_frame();
    // Pulse and noise timers run at half the CPU rate; the triangle and DMC timers at the full rate.
    if (odd_cycle_) {
        pulse_[0].timer_clock();
        pulse_[1].timer_clock();
        noise_.timer_clock();
    }
    tri_.timer_clock();
    dmc_clock();
    odd_cycle_ = !odd_cycle_;
}

// ---------------------------------------------------------------- output

float Apu::OutputFilter::process(float x) {
    // Coefficients for a 44.1 kHz rate: a = exp(-2*pi*fc/fs) for the high-passes, alpha for the low-pass.
    constexpr float kTwoPi = 6.28318530f;
    static const float fs = static_cast<float>(kSampleRate);
    static const float a90 = std::exp(-kTwoPi * 90.0f / fs);
    static const float a440 = std::exp(-kTwoPi * 440.0f / fs);
    static const float alpha = 1.0f - std::exp(-kTwoPi * 14000.0f / fs);

    float y = a90 * (hp90_y + x - hp90_x);
    hp90_x = x;
    hp90_y = y;
    x = y;
    y = a440 * (hp440_y + x - hp440_x);
    hp440_x = x;
    hp440_y = y;
    lp_y += alpha * (y - lp_y);
    return lp_y;
}

float Apu::mix() const {
    // NESdev nonlinear mixer approximation for the pulse and triangle/noise groups.
    const int p = pulse_[0].output() + pulse_[1].output();
    const float pulse_out = p == 0 ? 0.0f : 95.88f / (8128.0f / static_cast<float>(p) + 100.0f);
    const int t = tri_.output();
    const int n = noise_.output();
    const int d = dmc_.level;
    float tnd_out = 0.0f;
    if (t != 0 || n != 0 || d != 0) {
        const float denom = static_cast<float>(t) / 8227.0f + static_cast<float>(n) / 12241.0f +
                            static_cast<float>(d) / 22638.0f;
        tnd_out = 159.79f / (1.0f / denom + 100.0f);
    }
    return pulse_out + tnd_out;  // roughly 0..1, before the output filters
}

void Apu::set_rate_scale(double scale) {
    // Re-anchor at the current cycle so the output position stays continuous.
    const double base = static_cast<double>(kSampleRate) / static_cast<double>(kCpuHz);
    pos_anchor_ = pos_anchor_ + static_cast<double>(t_ - t_anchor_) * rate_;
    t_anchor_ = t_;
    rate_ = base * scale;
}

void Apu::render(int64_t until_cyc, std::vector<int16_t>& out) {
    while (t_ < until_cyc) {
        while (!pending_.empty() && pending_.front().cyc <= t_) {
            apply(pending_.front().addr, pending_.front().val);
            pending_.pop_front();
        }
        step();
        // Each change of the mixer adds a band-limited step at its exact output position.
        const float x = mix();
        const float dx = x - prev_mix_;
        prev_mix_ = x;
        const double pos = pos_anchor_ + static_cast<double>(t_ - t_anchor_) * rate_;
        if (dx != 0.0f) {
            const int64_t o = static_cast<int64_t>(std::floor(pos));
            const int p = static_cast<int>((pos - static_cast<double>(o)) * kPhases);
            const StepKernel& kern = step_kernel();
            for (int k = 0; k < kTaps; k++) {
                buf_[(o + k - kHalfTaps) & kBufMask] += dx * kern.h[p][k];
            }
        }
        // Output n is final once no later change can reach it (changes land at or after floor(pos)).
        const int64_t settled = static_cast<int64_t>(std::floor(pos)) - kHalfTaps;
        while (next_emit_ < settled) {
            float& slot = buf_[next_emit_ & kBufMask];
            acc_ += slot;
            slot = 0.0f;
            const float v = std::clamp(filter_.process(static_cast<float>(acc_)), -1.0f, 1.0f);
            out.push_back(static_cast<int16_t>(v * 24000.0f));
            next_emit_++;
        }
        t_++;
    }
}
