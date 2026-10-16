#include "runtime/apu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>

#include "runtime/nes_apu/Blip_Buffer.h"
#include "runtime/nes_apu/Nes_Apu.h"

struct Apu::Impl {
    Nes_Apu apu;
    Blip_Buffer buf;
    int64_t frame_start = 0;      // absolute CPU cycle the Blargg frame started at
    int64_t rendered_until = 0;   // last absolute cycle rendered
    double rate_scale = 1.0;

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

Apu::Apu() : impl_(new Impl) {
    reset();
}

Apu::~Apu() = default;

void Apu::reset() {
    Impl& d = *impl_;
    d.pending.clear();
    d.frame_start = 0;
    d.rendered_until = 0;
    // Linear path only: one shared buffer, per-oscillator linear gains.
    // Never call enable_nonlinear(); Nonlinear_Buffer is not compiled in.
    d.apu.output(&d.buf);
    d.buf.clock_rate(kCpuHz);
    // NOTE: Blip_Buffer's default length (blip_default_length = 0) computes the
    // buffer size from ULONG_MAX, which overflows to gigabytes on 64-bit
    // (immediate crash/OOM on launch). Use an explicit length instead: one
    // emulated frame is ~20 ms, so 500 ms holds plenty while staying small.
    if (const char* err = d.buf.sample_rate(kSampleRate, 500)) {
        std::fprintf(stderr, "apu: Blip_Buffer::sample_rate failed: %s\n", err);
    }
    d.apu.volume(1.0);
    d.apu.reset(false, 0);
    d.apu.dmc_reader(&dmc_reader_tramp, this);
    d.buf.clear();
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
    d.buf.end_frame(rel_end);
    d.frame_start = until_cyc;
    d.rendered_until = until_cyc;

    const long avail = d.buf.samples_avail();
    if (avail <= 0) return;
    std::vector<int16_t> raw(static_cast<size_t>(avail));
    const long got = d.buf.read_samples(raw.data(), avail);
    if (got <= 0) return;
    raw.resize(static_cast<size_t>(got));

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
