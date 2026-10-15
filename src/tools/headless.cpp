// Headless runner: loads the asset pack, runs the translated game for N frames
// with scripted input, and writes the final frame as PNG. Used for verification
// without a window.
//
//   smb3_headless --assets assets --frames 600 --out frame.png [--press start@300]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "runtime/machine.h"

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

}  // namespace

int main(int argc, char** argv) {
    std::string assets = "assets";
    std::string out = "frame.png";
    int frames = 300;
    int press_start_at = -1;
    int press_start_len = 6;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--assets" && i + 1 < argc) assets = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (arg == "--press-start" && i + 1 < argc) press_start_at = std::atoi(argv[++i]);
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

    for (int f = 0; f < frames; f++) {
        m->joy[0] = 0;
        if (press_start_at >= 0 && f >= press_start_at && f < press_start_at + press_start_len) m->joy[0] = 0x08;
        m->run_frame();
    }
    if (!write_png(out.c_str(), m->framebuf, 256, 240)) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("ran %d frames, wrote %s (pc=$%04X, cyc=%lld)\n", frames, out.c_str(),
                static_cast<unsigned>(m->pc), static_cast<long long>(m->cyc));
    delete m;
    return 0;
}
