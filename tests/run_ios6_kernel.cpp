// The real-kernel rig: boot Apple's own kernelcache through the emulator and
// report, in the kernel's own terms, where the machine stops.
//
// This is the research harness behind the "verified run" of the README: it
// fetches nothing, stores nothing and knows no keys -- the kernel cache and
// (for an encrypted one) the IV and key are arguments, from your own
// research, exactly like the app's install screen asks the user.
//
// Build (the same core the rig compiles):
//   g++ -std=c++17 -O2 -Wall -Wextra -Inative/include
//       tests/run_ios6_kernel.cpp native/memory/*.cpp native/cpu/*.cpp
//       native/image/*.cpp native/gpu/*.cpp native/core/*.cpp -o run_ios6_kernel
//
// Run:
//   ./run_ios6_kernel kernelcache.release.n90 [iv-hex key-hex]
//       [--steps 100000000] [--r0 0x0] [--trace N]
//
//   --steps   the instruction budget (default 100M, a few minutes of host
//             time -- the interpreter runs ~100 MIPS on a desktop core)
//   --r0      the value the boot CPU starts with in r0 (iBoot hands the
//             boot_args pointer there; this machine has no iBoot yet)
//   --trace   print the PC every N million instructions

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hakios/arm7_cpu.h"
#include "hakios/guest_map.h"
#include "hakios/kernel_loader.h"
#include "hakios/storage_memory.h"

using hakios::Arm7Cpu;
using hakios::KernelLoadReport;
using hakios::KernelLoader;
using hakios::StorageMemory;
using Halt = Arm7Cpu::Halt;

static std::vector<uint8_t> read_file(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return {};
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> v((size_t)n);
    if (n > 0 && std::fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear();
    std::fclose(f);
    return v;
}

static bool hex_key(const char* s, uint8_t* out, size_t want) {
    if (std::strlen(s) != want * 2) return false;
    for (size_t i = 0; i < want; ++i) {
        unsigned v;
        if (std::sscanf(s + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <kernelcache> [iv-hex key-hex] [--steps N] [--r0 0x..] [--trace N]\n",
                    argv[0]);
        return 1;
    }
    const std::vector<uint8_t> kernel = read_file(argv[1]);
    if (kernel.empty()) {
        std::printf("cannot read %s\n", argv[1]);
        return 1;
    }
    KernelLoader::Key key;
    const KernelLoader::Key* keyp = nullptr;
    uint64_t steps = 100000000ull;
    uint32_t r0 = 0;
    uint64_t trace = 0;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
            steps = std::strtoull(argv[++i], nullptr, 0);
        } else if (std::strcmp(argv[i], "--r0") == 0 && i + 1 < argc) {
            r0 = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
        } else if (std::strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace = std::strtoull(argv[++i], nullptr, 0);
        } else if (keyp == nullptr && i + 1 < argc &&
                   std::strlen(argv[i]) == 32 && std::strlen(argv[i + 1]) == 64) {
            if (!hex_key(argv[i], key.iv, 16) || !hex_key(argv[i + 1], key.key, 32)) {
                std::printf("bad hex key/iv\n");
                return 1;
            }
            keyp = &key;
            ++i;
        }
    }

    StorageMemory mem;
    if (!mem.create("/tmp/hakios_kernel_guest.bin", hakios::map::kPhysSize)) {
        std::printf("cannot create the guest image: %s\n", mem.last_error().c_str());
        return 1;
    }
    KernelLoadReport report;
    std::string err;
    if (!KernelLoader::load(mem, kernel.data(), kernel.size(), keyp, &report, &err)) {
        std::printf("the loader refused the kernel: %s\n", err.c_str());
        std::printf("%s\n", report.describe().c_str());
        return 1;
    }
    std::printf("%s\n", report.describe().c_str());
    std::printf("booting: pc=0x%08X sp=0x%08X r0=0x%08X, %llu instructions\n",
                report.entry_pc, report.entry_sp, r0, (unsigned long long)steps);

    Arm7Cpu cpu(mem);
    cpu.reset(report.entry_pc, report.entry_sp);
    cpu.set_reg(0, r0);

    uint64_t done = 0;
    const uint64_t slice = 5000000ull;
    Halt h = Halt::kNone;
    while (done < steps) {
        const uint64_t n = (steps - done < slice) ? steps - done : slice;
        h = cpu.run(n);
        done += n;
        std::printf("\r  %lluM instructions, pc=0x%08X", (unsigned long long)(done / 1000000),
                    cpu.pc());
        std::fflush(stdout);
        if (trace && (done / trace) != ((done - n) / trace)) {
            std::printf("\n  [trace] pc=0x%08X insn=0x%08X\n",
                        cpu.pc(), mem.read32(cpu.pc()));
        }
        if (h != Halt::kNone) break;
    }
    std::printf("\n");
    std::printf("halt: %s (%s)\n", Arm7Cpu::halt_name(h), cpu.info().detail);
    std::printf("  pc=0x%08X insn=0x%08X executed=%llu\n", cpu.info().halt_pc,
                cpu.info().halt_insn, (unsigned long long)cpu.info().executed);
    std::printf("  cpsr=0x%08X mode=0x%02X\n", cpu.cpsr(), cpu.mode());
    return h == Halt::kNone ? 0 : 2;
}
