// Headless runner: loads the asset pack, runs the translated game for N frames
// with scripted input, and writes the final frame as PNG. Used for verification
// without a window.
//
//   smb3_headless --assets assets --frames 600 --out frame.png
//                 [--press-start N] [--wav out.wav]
//                 [--script "120:8,130:0,300:128"] [--shots 300,600] [--shot-dir DIR]
//                 [--widescreen] [--dump-state] [--trace N]
//
// --script holds each frame:buttons entry (decimal or 0x; bit 0 = A, 1 = B, 2 = Select,
// 3 = Start, 4 = Up, 5 = Down, 6 = Left, 7 = Right) from its frame until the next entry.
// --shots dumps that many-frames-in picture to DIR/fNNNNN.png. With --widescreen the final
// PNG and the shots are the 426x240 wide picture; --dump-state prints the level, scroll,
// split and a few per-scanline rows after the run; --trace N prints the state line every
// N frames.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "runtime/machine.h"
#include "smb3_syms.h"

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(1);
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

uint32_t crc_table[256];

void make_crc_table() {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

uint32_t crc32(const uint8_t* buf, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < len; i++) crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void put_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}

void write_chunk(std::FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hdr;
    put_be32(hdr, static_cast<uint32_t>(data.size()));
    std::fwrite(hdr.data(), 1, 4, f);
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    std::fwrite(body.data(), 1, body.size(), f);
    uint32_t c = crc32(body.data(), body.size()) ^ 0xFFFFFFFFu;
    std::vector<uint8_t> tail;
    put_be32(tail, c);
    std::fwrite(tail.data(), 1, 4, f);
}

// PNG with stored (uncompressed) deflate blocks: no zlib dependency.
bool write_png(const char* path, const uint32_t* rgba, int w, int h) {
    make_crc_table();
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (w * 4 + 1));
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        for (int x = 0; x < w; x++) {
            uint32_t p = rgba[y * w + x];
            raw.push_back(p & 0xFF);
            raw.push_back((p >> 8) & 0xFF);
            raw.push_back((p >> 16) & 0xFF);
            raw.push_back(0xFF);
        }
    }
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size() || raw.empty()) {
        size_t n = raw.size() - pos;
        if (n > 65535) n = 65535;
        bool last = (pos + n) >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(n & 0xFF);
        z.push_back(n >> 8);
        z.push_back(~n & 0xFF);
        z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put_be32(z, (b << 16) | a);

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put_be32(ihdr, static_cast<uint32_t>(w));
    put_be32(ihdr, static_cast<uint32_t>(h));
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    write_chunk(f, "IHDR", ihdr);
    write_chunk(f, "IDAT", z);
    write_chunk(f, "IEND", {});
    std::fclose(f);
    return true;
}

void write_u16le(std::FILE* f, uint16_t x) {
    std::fputc(x & 0xFF, f);
    std::fputc(x >> 8, f);
}

void write_u32le(std::FILE* f, uint32_t x) {
    std::fputc(x & 0xFF, f);
    std::fputc((x >> 8) & 0xFF, f);
    std::fputc((x >> 16) & 0xFF, f);
    std::fputc(x >> 24, f);
}

// Mono int16 WAV at the APU output rate, for listening checks.
bool write_wav(const char* path, const std::vector<int16_t>& samples) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const uint32_t rate = 48000;
    const uint32_t bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    std::fwrite("RIFF", 1, 4, f);
    write_u32le(f, 36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    write_u32le(f, 16);
    write_u16le(f, 1);      // PCM
    write_u16le(f, 1);      // mono
    write_u32le(f, rate);
    write_u32le(f, rate * sizeof(int16_t));
    write_u16le(f, sizeof(int16_t));
    write_u16le(f, 16);
    std::fwrite("data", 1, 4, f);
    write_u32le(f, bytes);
    std::fwrite(samples.data(), sizeof(int16_t), samples.size(), f);
    std::fclose(f);
    return true;
}

// "a:m,b:n": at frame a the controller holds mask m until the next entry.
bool parse_script(const std::string& spec, std::vector<std::pair<int, int>>* events) {
    size_t pos = 0;
    while (pos <= spec.size() && !spec.empty()) {
        const size_t comma = spec.find(',', pos);
        const std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const size_t colon = item.find(':');
        if (colon == std::string::npos) return false;
        char* end = nullptr;
        const long frame = std::strtol(item.substr(0, colon).c_str(), &end, 0);
        if (*end != '\0' || frame < 0) return false;
        const long mask = std::strtol(item.substr(colon + 1).c_str(), &end, 0);
        if (*end != '\0' || mask < 0 || mask > 255) return false;
        events->emplace_back(static_cast<int>(frame), static_cast<int>(mask));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    std::stable_sort(events->begin(), events->end());
    return true;
}

bool parse_frames(const std::string& spec, std::vector<int>* frames) {
    size_t pos = 0;
    while (pos <= spec.size() && !spec.empty()) {
        const size_t comma = spec.find(',', pos);
        const std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        char* end = nullptr;
        const long frame = std::strtol(item.c_str(), &end, 0);
        if (*end != '\0' || frame <= 0) return false;
        frames->push_back(static_cast<int>(frame));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    std::stable_sort(frames->begin(), frames->end());
    return true;
}

// "F1[-F2]:ADDR=VAL[,ADDR=VAL...]": hold RAM bytes at fixed values over a frame range.
// Diagnostic only: used to e.g. keep Player_FlashInv topped up so capture runs survive.
struct Poke {
    int first, last;
    uint16_t addr;
    uint8_t val;
};

bool parse_poke(const std::string& spec, std::vector<Poke>* pokes) {
    const size_t colon = spec.find(':');
    if (colon == std::string::npos) return false;
    const std::string range = spec.substr(0, colon);
    const size_t dash = range.find('-');
    char* end = nullptr;
    const long first = std::strtol(range.substr(0, dash).c_str(), &end, 0);
    if (*end != '\0' || first < 0) return false;
    long last = first;
    if (dash != std::string::npos) {
        last = std::strtol(range.substr(dash + 1).c_str(), &end, 0);
        if (*end != '\0' || last < first) return false;
    }
    size_t pos = colon + 1;
    while (pos < spec.size()) {
        const size_t comma = spec.find(',', pos);
        const std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const size_t eq = item.find('=');
        if (eq == std::string::npos) return false;
        const long addr = std::strtol(item.substr(0, eq).c_str(), &end, 0);
        if (*end != '\0' || addr < 0 || addr > 0xFFFF) return false;
        const long val = std::strtol(item.substr(eq + 1).c_str(), &end, 0);
        if (*end != '\0' || val < 0 || val > 255) return false;
        pokes->push_back({static_cast<int>(first), static_cast<int>(last),
                          static_cast<uint16_t>(addr), static_cast<uint8_t>(val)});
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return true;
}

void poke_byte(Machine& m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000) m.ram[addr & 0x7FF] = val;
    else if (addr >= 0x6000 && addr < 0x8000) m.sram[addr - 0x6000] = val;
}

void print_state(Machine& m, int frame) {
    const auto rd16 = [&](uint16_t lo, uint16_t hi) { return m.rd(lo) | (m.rd(hi) << 8); };
    std::printf("state f%05d: enter=%u tileset=%u width=%u vert7=%u cam=$%04X vscroll=$%04X "
                "player=$%04X/$%04X psx=%d psy=%d mappos=$%04X/%04X maptile=$%02X mapop=$%02X "
                "pan=$%02X mapmove=$%02X lives=%u endl=%u halt=%u exitmap=%u mret=%u inair=%u "
                "suit=%u padh=$%02X padi=$%02X dying=%u hgame=%u pipe=%u vibe=%u xvel=%d yvel=%d "
                "tim=%u/%u/%u est=%u evt=%u aboard=%u qsuit=%u chgreset=%u "
                "hud_top=%d wide_frame_x=%d wide_left=%d\n",
                frame,
                m.rd(smb3::World_EnterState), m.rd(smb3::Level_Tileset), m.rd(smb3::Level_Width),
                m.rd(smb3::Level_7Vertical), rd16(smb3::Horz_Scroll, smb3::Horz_Scroll_Hi),
                rd16(smb3::Level_VertScroll, smb3::Level_VertScrollH),
                rd16(smb3::Player_X, smb3::Player_XHi), rd16(smb3::Player_Y, smb3::Player_YHi),
                static_cast<int8_t>(m.rd(smb3::Player_SpriteX)),
                static_cast<int8_t>(m.rd(smb3::Player_SpriteY)),
                rd16(smb3::World_Map_X, smb3::World_Map_XHi), m.rd(smb3::World_Map_Y),
                m.rd(smb3::World_Map_Tile), m.rd(smb3::Map_Operation), m.rd(smb3::Map_Pan_Count),
                m.rd(smb3::World_Map_Move), m.rd(smb3::Player_Lives), m.rd(smb3::Player_EndLevel),
                m.rd(smb3::Player_HaltTick), m.rd(smb3::Level_ExitToMap),
                m.rd(smb3::Map_ReturnStatus), m.rd(smb3::Player_InAir), m.rd(smb3::Player_Suit),
                m.rd(smb3::Pad_Holding), m.rd(smb3::Pad_Input), m.rd(smb3::Player_IsDying),
                m.rd(smb3::Player_HaltGame), m.rd(smb3::Level_PipeMove), m.sram[0x1CF4],
                static_cast<int8_t>(m.rd(smb3::Player_XVel)),
                static_cast<int8_t>(m.rd(smb3::Player_YVel)),
                m.rd(smb3::Level_TimerMSD), m.rd(smb3::Level_TimerMid), m.rd(smb3::Level_TimerLSD),
                m.rd(smb3::Level_TimerEn),
                m.rd(smb3::Event_Countdown), m.rd(smb3::Player_AboveTop),
                m.rd(smb3::Player_QueueSuit), m.rd(smb3::Level_ChangeReset),
                m.hud_top, m.wide_frame_x, m.wide_left);
}

// One line per 16x16 block row of the loaded level: '..' empty, two hex digits per block.
// Tile_Mem = $6000 (sram[0]), 15 screens of 16x27 blocks, row-major within a screen.
void print_level(Machine& m) {
    const int screens = std::min(static_cast<int>(m.rd(smb3::Level_Width)) + 1, 15);
    static const char* kHex = "0123456789ABCDEF";
    for (int s = 0; s < screens; s++) {
        std::printf("screen %d (x %d..%d):\n", s, s * 256, s * 256 + 255);
        for (int r = 0; r < 27; r++) {
            char line[33];
            for (int c = 0; c < 16; c++) {
                const uint8_t t = m.sram[s * 0x1B0 + r * 16 + c];
                line[2 * c] = t == 0 ? '.' : kHex[t >> 4];
                line[2 * c + 1] = t == 0 ? '.' : kHex[t & 0xF];
            }
            line[32] = '\0';
            std::printf("  %2d %s\n", r, line);
        }
    }
}

void print_rows(const Machine& m) {
    const int rows[] = {0, 8, 16, 24, 31, 32, 33, 40, 48, 120, 232};
    for (int y : rows) {
        const Machine::HudLine& h = m.hud_lines[y];
        const unsigned world_y =
            static_cast<unsigned>(((h.v >> 11) & 1) * 240 + ((h.v >> 5) & 0x1F) * 8 + ((h.v >> 12) & 7));
        std::printf("  row %3d: v=$%04X world_y=%3u on=%d mask=$%02X ctrl=$%02X pal0=$%02X "
                    "bank=%u,%u,%u,%u\n",
                    y, h.v, world_y, h.on ? 1 : 0, h.mask, h.ctrl, h.palram[0], h.chr_slot[0],
                    h.chr_slot[1], h.chr_slot[2], h.chr_slot[3]);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string assets = "assets";
    std::string out = "frame.png";
    std::string wav_out;
    std::string shot_dir = ".";
    std::string script_spec;
    std::vector<int> shot_frames;
    std::vector<std::pair<int, int>> script;
    std::vector<Poke> pokes;
    int frames = 300;
    int press_start_at = -1;
    int press_start_len = 6;
    bool widescreen = false;
    bool dump_state = false;
    bool dump_level = false;
    int trace_every = 0;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--assets" && i + 1 < argc) assets = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (arg == "--press-start" && i + 1 < argc) press_start_at = std::atoi(argv[++i]);
        else if (arg == "--wav" && i + 1 < argc) wav_out = argv[++i];
        else if (arg == "--widescreen") widescreen = true;
        else if (arg == "--dump-state") dump_state = true;
        else if (arg == "--dump-level") dump_level = true;
        else if (arg == "--trace" && i + 1 < argc) trace_every = std::atoi(argv[++i]);
        else if (arg == "--script" && i + 1 < argc) script_spec = argv[++i];
        else if (arg == "--shots" && i + 1 < argc) {
            if (!parse_frames(argv[++i], &shot_frames)) {
                std::fprintf(stderr, "bad --shots list\n");
                return 1;
            }
        } else if (arg == "--shot-dir" && i + 1 < argc) {
            shot_dir = argv[++i];
        } else if (arg == "--poke" && i + 1 < argc) {
            if (!parse_poke(argv[++i], &pokes)) {
                std::fprintf(stderr, "bad --poke spec (want \"f1[-f2]:addr=val,...\")\n");
                return 1;
            }
        } else {
            std::fprintf(stderr, "unrecognized argument: %s\n", arg.c_str());
            return 1;
        }
    }
    if (!script_spec.empty() && !parse_script(script_spec, &script)) {
        std::fprintf(stderr, "bad --script spec (want \"frame:mask,...\")\n");
        return 1;
    }

    std::vector<uint8_t> prg = read_file(assets + "/prg.bin");
    std::vector<uint8_t> chr = read_file(assets + "/chr.bin");
    if (prg.size() != 256 * 1024 || chr.size() != 128 * 1024) {
        std::fprintf(stderr, "asset pack has wrong sizes\n");
        return 1;
    }

    Machine* m = new Machine();
    m->prg = prg.data();
    m->chr = chr.data();
    m->reset();

    if (widescreen) m->set_widescreen(true);

    // The APU applies its register writes and advances only inside render(), so call it every
    // frame as the desktop app does. Samples are kept only when --wav asks for them.
    std::vector<int16_t> samples;
    std::vector<int16_t> all_samples;
    if (!wav_out.empty()) all_samples.reserve(static_cast<size_t>(frames) * 900);
    size_t script_pos = 0, shot_pos = 0;
    int script_mask = 0;
    for (int f = 0; f < frames; f++) {
        while (script_pos < script.size() && script[script_pos].first <= f) {
            script_mask = script[script_pos].second;
            script_pos++;
        }
        m->joy[0] = static_cast<uint8_t>(script_mask);
        if (press_start_at >= 0 && f >= press_start_at && f < press_start_at + press_start_len) {
            m->joy[0] = 0x08;
        }
        for (const Poke& p : pokes) {
            if (f >= p.first && f <= p.last) poke_byte(*m, p.addr, p.val);
        }
        m->run_frame();
        if (widescreen) m->wide_compose();  // exactly how the desktop app presents the frame
        samples.clear();
        m->render_audio(m->cyc, samples);
        if (!wav_out.empty()) all_samples.insert(all_samples.end(), samples.begin(), samples.end());
        while (shot_pos < shot_frames.size() && shot_frames[shot_pos] <= f + 1) {
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/f%05d.png", shot_dir.c_str(), shot_frames[shot_pos]);
            const bool ok = widescreen
                                ? write_png(path, m->wide_fb.data(), Machine::kWideW, 240)
                                : write_png(path, m->framebuf, 256, 240);
            if (!ok) {
                std::fprintf(stderr, "cannot write %s\n", path);
                return 1;
            }
            std::printf("wrote %s\n", path);
            shot_pos++;
        }
        if (trace_every > 0 && (f + 1) % trace_every == 0) print_state(*m, f + 1);
    }
    if (!wav_out.empty() && !write_wav(wav_out.c_str(), all_samples)) {
        std::fprintf(stderr, "cannot write %s\n", wav_out.c_str());
        return 1;
    }
    const bool wrote = widescreen ? write_png(out.c_str(), m->wide_fb.data(), Machine::kWideW, 240)
                                  : write_png(out.c_str(), m->framebuf, 256, 240);
    if (!wrote) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("ran %d frames, wrote %s (pc=$%04X, cyc=%lld)\n", frames, out.c_str(),
                static_cast<unsigned>(m->pc), static_cast<long long>(m->cyc));
    if (dump_state) {
        print_state(*m, frames);
        print_rows(*m);
    }
    if (dump_level) print_level(*m);
    delete m;
    return 0;
}
