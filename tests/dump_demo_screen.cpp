// The demo kernel's screen, as pixels on a desktop: load the bundled
// self-test kernel through the real engine, run it (to the halt, or a
// given instruction budget to catch the boot screen mid-show), and write
// the guest's 640x960 RGB565 framebuffer out as a binary PPM (P6).
//
//   tests/dump_demo_screen.sh                       final state
//   tests/dump_demo_screen.sh - 40000000 mid.ppm    after 40M instructions
//
// Part of the stage-8 rig's family (built the same way, run by hand).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hakios/emulator.h"
#include "hakios/guest_map.h"

using hakios::Emulator;

int main(int argc, char** argv) {
    const char* img3 = argc > 1 && strcmp(argv[1], "-") ? argv[1]
                                                        : "app/src/main/assets/demo-kernel.img3";
    const char* out = argc > 2 ? argv[2] : "demo-screen.ppm";
    uint64_t budget = 512ull * 1000 * 1000;
    if (argc > 3) budget = std::strtoull(argv[3], nullptr, 10);

    std::FILE* f = std::fopen(img3, "rb");
    if (!f) {
        std::printf("cannot open %s (run from the repository root)\n", img3);
        return 1;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> kernel(size);
    if (std::fread(kernel.data(), 1, size, f) != size_t(size)) {
        std::printf("short read on %s\n", img3);
        std::fclose(f);
        return 1;
    }
    std::fclose(f);

    Emulator e;
    std::string err;
    if (!e.create("/tmp/hakios-dump-guest.img", &err) ||
        !e.load_kernel(kernel.data(), kernel.size(), nullptr, nullptr, &err)) {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }
    const uint64_t ran = e.run_sync(budget);
    const Emulator::Status s = e.status();
    std::printf("ran %llu instructions, state=%d halt=%d (executed=%llu)\n",
                (unsigned long long)ran, int(s.state), int(s.halt.reason),
                (unsigned long long)s.executed);

    // RGB565 -> P6, one byte a channel
    std::vector<unsigned char> rgb(size_t(hakios::map::kScreenWidth) *
                                   hakios::map::kScreenHeight * 3);
    const uint16_t* px = reinterpret_cast<const uint16_t*>(
        e.memory().base() + hakios::map::kVramBase);
    for (size_t i = 0; i < rgb.size() / 3; i++) {
        const uint16_t v = px[i];
        rgb[i * 3 + 0] = (unsigned char)(((v >> 11) & 0x1F) << 3);
        rgb[i * 3 + 1] = (unsigned char)(((v >> 5) & 0x3F) << 2);
        rgb[i * 3 + 2] = (unsigned char)((v & 0x1F) << 3);
    }
    std::FILE* o = std::fopen(out, "wb");
    if (!o) {
        std::printf("cannot write %s\n", out);
        return 1;
    }
    std::fprintf(o, "P6\n%u %u\n255\n", hakios::map::kScreenWidth,
                 hakios::map::kScreenHeight);
    std::fwrite(rgb.data(), 1, rgb.size(), o);
    std::fclose(o);
    std::printf("wrote %s\n", out);
    return 0;
}
