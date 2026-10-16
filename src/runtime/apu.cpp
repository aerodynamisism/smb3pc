#include "runtime/apu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>

#include "runtime/nes_apu/Blip_Buffer.h"
#include "runtime/nes_apu/Nes_Apu.h"

struct Apu::Impl {
    Nes_Apu apu;
    Blip_Buffer buf;          // linear mix: all oscillators land here
    Blip_Buffer chan[5];      // nonlinear mix: one buffer per oscillator
                              // (0 square1, 1 square2, 2 triangle, 3 noise, 4 DMC)
    int64_t frame_start = 0;      // absolute CPU cycle the Blargg frame started at
    int64_t rendered_until = 0;   // last absolute cycle rendered
    double rate_scale = 1.0;
    Mixing mixing = Mixing::Linear;
    // DC estimate per channel. Blip_Buffers high-pass their output, but the
    // hardware mixer curves need absolute channel levels, so the removed DC
    // is restored with the matching one-pole low-pass (bass_shift 9, see
    // Blip_Buffer::bass_freq with the 16 Hz default at 48 kHz).
    double chan_dc[5] = {};
    // Output DC blocker state. The hardware curves produce a unipolar level
    // (like the 2A03 pin); the NES high-passes it downstream, so the blocker
    // restores the DC-free property the linear path gets from Blip_Buffers.
    double out_dc = 0.0;

    struct PendingWrite {
        int64_t cyc;
        uint16_t addr;
        uint8_t val;
    };
    std::deque<PendingWrite> pending;
};

namespace {

int dmc_reader_tramp(void* user_data, cpu_addr_t addr) {
    Apu* self = static_cast<Apu*>(user_data);
    if (!self->read_cpu) return 0x55;  // flat sample when no memory hook yet
    return self->read_cpu(static_cast<uint16_t>(addr));
}

}  // namespace

// Hardware mixer curves (2A03 output stage). Levels are in DAC units:
// squares and triangle/noise 0..15, DMC 0..127 (linear DAC; the DMC DAC-table
// curve is not modelled because dmc.nonlinear is only settable through
// enable_nonlinear(), which this wrapper never calls).
// The per-channel Blip_Buffers high-pass their output, so each channel's
// removed DC is restored with the matching one-pole low-pass before the
// curves are applied; without that the curves would operate on the wrong
// levels and silence would not stay silent.
void Apu::nonlinear_mix(Apu::Impl& d, std::vector<int16_t>& out) {
    // Inverse of the volume(1.0) gains, per channel DAC range.
    static const double kInv[5] = {
        15.0 / (32768.0 * 0.1128),   // square 1
        15.0 / (32768.0 * 0.1128),   // square 2
        15.0 / (32768.0 * 0.12765),  // triangle
        15.0 / (32768.0 * 0.0741),   // noise
        127.0 / (32768.0 * 0.42545),  // DMC
    };
    static const double kDcStep = 1.0 / 512.0;  // bass_shift 9
    static const double kMaster = 30000.0;      // headroom below int16 full scale

    int16_t tmp[5][2048];
    long avail = d.chan[0].samples_avail();
    for (int i = 1; i < 5; i++) avail = std::min(avail, d.chan[i].samples_avail());
    out.clear();
    while (avail > 0) {
        long n = std::min<long>(avail, 2048);
        for (int i = 0; i < 5; i++) {
            if (d.chan[i].read_samples(tmp[i], n) != n) return;
        }
        avail -= n;
        for (long s = 0; s < n; s++) {
            double level[5];
            for (int i = 0; i < 5; i++) {
                d.chan_dc[i] += (tmp[i][s] - d.chan_dc[i]) * kDcStep;
                double v = (tmp[i][s] + d.chan_dc[i]) * kInv[i];
                level[i] = v > 0.0 ? v : 0.0;
            }
            const double pulse_sum = level[0] + level[1];
            const double pulse_out = pulse_sum > 0.0 ? 95.88 / (8128.0 / pulse_sum + 100.0) : 0.0;
            const double tnd_in = level[2] / 8227.0 + level[3] / 12241.0 + level[4] / 22638.0;
            const double tnd_out = tnd_in > 0.0 ? 159.79 / (1.0 / tnd_in + 100.0) : 0.0;
            // DC blocker: the curves output a unipolar level, so strip the DC
            // the way the NES high-pass (and the linear path) does.
            const double uni = kMaster * (pulse_out + tnd_out);
            d.out_dc += (uni - d.out_dc) * (1.0 / 1024.0);
            long mixed = std::lround(uni - d.out_dc);
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            out.push_back(static_cast<int16_t>(mixed));
        }
    }
}

Apu::Apu() : impl_(new Impl) {
    reset();
}

Apu::~Apu() = default;

void Apu::reset() {
    Impl& d = *impl_;
    d.pending.clear();
    d.frame_start = 0;
    d.rendered_until = 0;
    for (int i = 0; i < 5; i++) {
        d.chan_dc[i] = 0.0;
        d.apu.osc_output(i, d.mixing == Mixing::Nonlinear ? &d.chan[i] : nullptr);
    }
    d.out_dc = 0.0;
    if (d.mixing == Mixing::Linear) {
        // Linear path only: one shared buffer, per-oscillator linear gains.
        d.apu.output(&d.buf);
    }
    // NOTE: Blip_Buffer's default length (blip_default_length = 0) computes the
    // buffer size from ULONG_MAX, which overflows to gigabytes on 64-bit
    // (immediate crash/OOM on launch). Use an explicit length instead: one
    // emulated frame is ~20 ms, so 500 ms holds plenty while staying small.
    d.buf.clock_rate(kCpuHz);
    if (const char* err = d.buf.sample_rate(kSampleRate, 500)) {
        std::fprintf(stderr, "apu: Blip_Buffer::sample_rate failed: %s\n", err);
    }
    for (int i = 0; i < 5; i++) {
        d.chan[i].clock_rate(kCpuHz);
        if (const char* err = d.chan[i].sample_rate(kSampleRate, 500)) {
            std::fprintf(stderr, "apu: channel Blip_Buffer::sample_rate failed: %s\n", err);
        }
    }
    d.apu.volume(1.0);
    d.apu.reset(false, 0);
    d.apu.dmc_reader(&dmc_reader_tramp, this);
    d.buf.clear();
    for (int i = 0; i < 5; i++) d.chan[i].clear();
}

void Apu::set_mixing(Mixing mode) {
    Impl& d = *impl_;
    if (mode == d.mixing) return;
    d.mixing = mode;
    // Re-route only; oscillator last_amp state is kept, so no levels jump.
    // Buffers are cleared because their old content belongs to the other mode.
    for (int i = 0; i < 5; i++) {
        d.apu.osc_output(i, mode == Mixing::Nonlinear ? &d.chan[i] : nullptr);
        d.chan_dc[i] = 0.0;
        d.chan[i].clear();
    }
    d.out_dc = 0.0;
    if (mode == Mixing::Linear) d.apu.output(&d.buf);
    d.buf.clear();
}

Mixing Apu::mixing() const {
    return impl_->mixing;
}

void Apu::write(uint16_t addr, uint8_t val, int64_t cyc) {
    impl_->pending.push_back(Impl::PendingWrite{cyc, addr, val});
}

uint8_t Apu::read_status(int64_t cyc) {
    Impl& d = *impl_;
    if (cyc < d.frame_start) cyc = d.frame_start;
    while (!d.pending.empty() && d.pending.front().cyc <= cyc) {
        const auto w = d.pending.front();
        d.pending.pop_front();
        long rel = static_cast<long>(w.cyc - d.frame_start);
        if (rel < 0) rel = 0;
        d.apu.write_register(rel, w.addr, w.val);
    }
    long rel = static_cast<long>(cyc - d.frame_start);
    if (rel < 0) rel = 0;
    return static_cast<uint8_t>(d.apu.read_status(rel) & 0xFF);
}

void Apu::set_rate_scale(double scale) {
    if (!std::isfinite(scale)) return;
    if (scale < 0.5 || scale > 2.0) return;
    impl_->rate_scale = scale;
}

void Apu::render(int64_t until_cyc, std::vector<int16_t>& out) {
    Impl& d = *impl_;
    if (until_cyc <= d.rendered_until) return;
    if (until_cyc < d.frame_start) {
        d.frame_start = until_cyc;
        d.rendered_until = until_cyc;
        return;
    }
    while (!d.pending.empty() && d.pending.front().cyc <= until_cyc) {
        const auto w = d.pending.front();
        d.pending.pop_front();
        long rel = static_cast<long>(w.cyc - d.frame_start);
        if (rel < 0) rel = 0;
        d.apu.write_register(rel, w.addr, w.val);
    }
    long rel_end = static_cast<long>(until_cyc - d.frame_start);
    if (rel_end < 0) rel_end = 0;
    d.apu.end_frame(rel_end);
    // Only the buffers in use advance; the idle ones are cleared so unread
    // samples cannot pile past the end of the buffer.
    if (d.mixing == Mixing::Nonlinear) {
        d.buf.clear();
        for (int i = 0; i < 5; i++) d.chan[i].end_frame(rel_end);
    } else {
        d.buf.end_frame(rel_end);
        for (int i = 0; i < 5; i++) d.chan[i].clear();
    }
    d.frame_start = until_cyc;
    d.rendered_until = until_cyc;

    std::vector<int16_t> raw;
    if (d.mixing == Mixing::Nonlinear) {
        nonlinear_mix(d, raw);
    } else {
        const long avail = d.buf.samples_avail();
        if (avail <= 0) return;
        raw.resize(static_cast<size_t>(avail));
        const long got = d.buf.read_samples(raw.data(), avail);
        if (got <= 0) return;
        raw.resize(static_cast<size_t>(got));
    }
    if (raw.empty()) return;

    const long got = static_cast<long>(raw.size());
    const double scale = d.rate_scale;
    if (std::fabs(scale - 1.0) < 1e-9) {
        out.insert(out.end(), raw.begin(), raw.end());
        return;
    }
    long want = static_cast<long>(std::lround(static_cast<double>(got) * scale));
    if (want < 1) want = 1;
    if (want > got * 2 + 32) want = got * 2 + 32;
    out.reserve(out.size() + static_cast<size_t>(want));
    if (got == 1 || want == 1) {
        out.insert(out.end(), static_cast<size_t>(want), raw[0]);
        return;
    }
    for (long j = 0; j < want; j++) {
        const double src = static_cast<double>(j) * (got - 1) / (want - 1);
        long i = static_cast<long>(src);
        if (i < 0) i = 0;
        if (i >= got - 1) i = got - 2;
        const double f = src - i;
        const double a = raw[static_cast<size_t>(i)];
        const double b = raw[static_cast<size_t>(i + 1)];
        out.push_back(static_cast<int16_t>(std::lround(a + (b - a) * f)));
    }
}
