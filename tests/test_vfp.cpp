// The VFP and banking tests: everything the kernel's own boot path needs
// beyond the stage-3 integer core -- VFPv3-D16 arithmetic, the system
// registers vfp_init reads, the loads and stores, the conversions, LDRD/
// STRD, the banked register file with its SPSR exception return, the
// user-bank LDM^/STM^, and CPS.
//
// Programs come from tests/gen/arm_progs.h (keystone-assembled, the same
// discipline as test_cpu). Float results are compared as raw bit patterns;
// sqrt(6) is the one value computed here, from the host's own conversion.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>

#include "gen/arm_progs.h"
#include "hakios/arm7_cpu.h"
#include "hakios/guest_map.h"
#include "hakios/storage_memory.h"

using hakios::Arm7Cpu;
using hakios::StorageMemory;
using Halt = Arm7Cpu::Halt;

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                   \
        }                                                                  \
    } while (0)

static const ArmProg* find_prog(const char* name) {
    for (size_t i = 0; i < kArmProgCount; ++i)
        if (std::strcmp(kArmProgs[i].name, name) == 0) return &kArmProgs[i];
    std::printf("FAIL: program %s not found in arm_progs.h\n", name);
    ++g_failures;
    return nullptr;
}

struct RunResult {
    StorageMemory mem;
    Arm7Cpu cpu;
    Halt halt = Halt::kNone;
    RunResult() : cpu(mem) {}
};

static bool run_program(const char* name, std::unique_ptr<RunResult>* out) {
    const ArmProg* p = find_prog(name);
    if (p == nullptr) return false;
    auto r = std::make_unique<RunResult>();
    const std::string img = std::string("physmem_") + name + ".bin";
    if (!r->mem.create(img, hakios::map::kPhysSize)) {
        std::printf("FAIL %s: cannot create guest image: %s\n", name,
                    r->mem.last_error().c_str());
        ++g_failures;
        return false;
    }
    if (!r->mem.copy_in(p->org, p->words, p->n_words * sizeof(p->words[0]))) {
        std::printf("FAIL %s: cannot load program\n", name);
        ++g_failures;
        return false;
    }
    r->cpu.reset(p->org);
    r->halt = r->cpu.run(10000);
    *out = std::move(r);
    return true;
}

static uint32_t f32bits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }

int main(int argc, char** argv) {
    if (argc > 1 && ::chdir(argv[1]) != 0) {
        std::printf("FAIL: cannot chdir to %s\n", argv[1]);
        return 1;
    }

    std::unique_ptr<RunResult> r;

    // -- vfp_arith ---------------------------------------------------------------
    if (run_program("vfp_arith", &r)) {
        CHECK(r->cpu.reg(0) == 0x40A00000);   // 2 + 3 = 5.0
        CHECK(r->cpu.reg(1) == 0x3F800000);   // 3 - 2 = 1.0
        CHECK(r->cpu.reg(2) == 0x40C00000);   // 2 * 3 = 6.0
        CHECK(r->cpu.reg(3) == 0x3FC00000);   // 3 / 2 = 1.5
        CHECK(r->cpu.reg(4) == f32bits(std::sqrt(6.0f)));  // vsqrt of 6.0
        CHECK(r->cpu.reg(5) == 0xBF800000);   // vneg of 1.0
        CHECK(r->cpu.reg(6) == 0x3F800000);   // vabs of -1.0
        CHECK(r->cpu.reg(7) == 0x40C00000);   // 0 + 2*3 = 6.0 (vmla from zero)
        CHECK(r->cpu.reg(8) == 0x40A00000);   // s16 + s1: the high bank works
        CHECK(r->halt == Halt::kUnknownInstruction);   // ran to the UDF
    }

    // -- vfp_d64 -------------------------------------------------------------------
    if (run_program("vfp_d64", &r)) {
        CHECK(r->cpu.reg(0) == 0x00000000);           // 3.0 low
        CHECK(r->cpu.reg(1) == 0x40080000);           // 3.0 high
        CHECK(r->cpu.reg(2) == 0x00000000);           // 1.0 low
        CHECK(r->cpu.reg(3) == 0x3FF00000);           // 1.0 high
        CHECK(r->cpu.reg(4) == 0x00000000);           // 2.0 low
        CHECK(r->cpu.reg(5) == 0x40000000);           // 2.0 high
        CHECK(r->cpu.reg(6) == 0x00000000);           // 2.0 low (vdiv)
        CHECK(r->cpu.reg(7) == 0x40000000);           // 2.0 high
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- vfp_ldst --------------------------------------------------------------------
    if (run_program("vfp_ldst", &r)) {
        CHECK(r->mem.read32(0x200000) == 0x40000000); // vstr of 2.0
        CHECK(r->mem.read32(0x200004) == 0x40400000); // vstmia, s1 = 3.0
        CHECK(r->mem.read32(0x200010) == 0x00000000); // vstr d0 low
        CHECK(r->mem.read32(0x200014) == 0x3FF00000); // vstr d0 high
        CHECK(r->cpu.reg(0) == 0x40000000);           // vldr
        CHECK(r->cpu.reg(1) == 0x40400000);           // vldr +4
        CHECK(r->cpu.reg(2) == 0x40000000);           // vldmia
        CHECK(r->cpu.reg(3) == 0x40400000);
        CHECK(r->cpu.reg(4) == 0x00000000);           // vldr d / vmov r,r,d
        CHECK(r->cpu.reg(5) == 0x3FF00000);
        CHECK(r->cpu.reg(6) == 0x00000000);           // vpush/vpop round trip
        CHECK(r->cpu.reg(7) == 0x3FF00000);
        CHECK(r->cpu.reg(13) == 0x201000);            // the stack is whole
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- vfp_sysregs: the identity vfp_init reads, and the compare path ---------------
    if (run_program("vfp_sysregs", &r)) {
        CHECK(r->cpu.reg(0) == 0x410330C0);           // FPSID: the Cortex-A8
        CHECK(r->cpu.reg(1) == 0x11110222);           // MVFR0
        CHECK(r->cpu.reg(2) == 0x00011111);           // MVFR1
        CHECK(r->cpu.reg(3) == 0x60000013);           // equal: Z + C
        CHECK(r->cpu.reg(4) == 0x80000013);           // less: N
        CHECK((r->cpu.reg(5) & 0xF0000000u) == 0x8000000000000000u >> 32);
        CHECK(r->cpu.reg(6) == 0x410330C0);           // FPEXC stored and read back
        CHECK(r->cpu.vfp_sysid(0) == 0x410330C0);     // the accessor agrees
        CHECK(r->cpu.vfp_sysid(7) == 0x11110222);
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- vfp_cvt: the conversions ------------------------------------------------------
    if (run_program("vfp_cvt", &r)) {
        CHECK(r->cpu.reg(1) == 0x40E00000);           // 7 -> 7.0f
        CHECK(r->cpu.reg(2) == 7);                    // and back
        CHECK(r->cpu.reg(3) == 2);                    // 1.5 rounds to 2
        CHECK(r->cpu.reg(4) == 1);                    // vcvtr: toward zero
        CHECK(r->cpu.reg(5) == 0x40E00000);           // f32 -> f64 -> f32
        CHECK(r->cpu.reg(7) == 0x4F800000);           // (float)0xFFFFFFFF
        CHECK(r->cpu.reg(8) == 0xFFFFFFFFu);          // saturated on the way back
        CHECK(r->cpu.reg(9) == 24);                   // fixed point #2: 6*4
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- ldrd_strd -----------------------------------------------------------------------
    if (run_program("ldrd_strd", &r)) {
        CHECK(r->cpu.reg(4) == 0x78563412);
        CHECK(r->cpu.reg(5) == 0x98760F0E);
        CHECK(r->cpu.reg(6) == 0x78563412);
        CHECK(r->cpu.reg(7) == 0x98760F0E);
        CHECK(r->mem.read32(0x200000) == 0x78563412);
        CHECK(r->mem.read32(0x200004) == 0x98760F0E);
        CHECK(r->mem.read32(0x200008) == 0x78563412);
        CHECK(r->mem.read32(0x20000C) == 0x98760F0E);
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- banked_modes: the banks, an SPSR, and the exception return ------------------------
    if (run_program("banked_modes", &r)) {
        CHECK(r->cpu.reg(1) == 0x1001);               // the svc bank of r13
        CHECK(r->cpu.reg(2) == 0x2002);               // the irq bank
        CHECK(r->cpu.reg(3) == 0x88);                 // the shared r8
        CHECK(r->cpu.reg(4) == 0x77);                 // the fiq bank of r8
        CHECK(r->cpu.reg(5) == 0x3301);               // the fiq bank of r13
        CHECK(r->cpu.reg(6) == 0x60000013);           // SPSR written and read
        CHECK(r->cpu.reg(8) == 0x60000013);           // movs pc restored Z + svc
        CHECK(r->cpu.mode() == 0x13);                 // ... and the mode
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- ldm_user_bank: LDM^/STM^ address the usr bank, not the current one ------------------
    if (run_program("ldm_user_bank", &r)) {
        CHECK(r->mem.read32(0x200000) == 0xBBBB0002); // what the last writer left
        CHECK(r->cpu.reg(3) == 0);                    // the svc r13 was untouched
        CHECK(r->cpu.reg(4) == 0xBBBB0002);           // the usr r13 got the load
        CHECK(r->cpu.mode() == 0x13);
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- cps_forms ---------------------------------------------------------------------------
    if (run_program("cps_forms", &r)) {
        CHECK(r->cpu.reg(0) == 0xD3);                 // cpsid if: I + F set
        CHECK(r->cpu.reg(1) == 0x53);                 // cpsie i: I cleared
        CHECK(r->cpu.reg(2) == 0x53);                 // cps #0x13 changed nothing
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    if (g_failures == 0) {
        std::printf("test_vfp: all checks passed\n");
        return 0;
    }
    std::printf("test_vfp: %d failure(s)\n", g_failures);
    return 1;
}
