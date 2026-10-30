// Stage 3 unit tests: the ARMv7 interpreter.
//
// Every program comes from tests/gen/arm_progs.h, assembled by keystone
// (scripts/gen_arm_tests.py) -- no hand-encoded instruction words anywhere
// except where keystone itself cannot express the encoding (RRX), which the
// generator packs as a documented raw word.
//
// Programs end with UDF; "halted on unknown instruction at the last word"
// therefore means "ran the whole program and stopped cleanly".

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
    for (size_t i = 0; i < kArmProgCount; ++i) {
        if (std::strcmp(kArmProgs[i].name, name) == 0) return &kArmProgs[i];
    }
    std::printf("FAIL: program %s not found in arm_progs.h\n", name);
    ++g_failures;
    return nullptr;
}

// Runs one generated program to completion on a fresh guest image and
// returns the cpu in its halted state for inspection. UDF placement is an
// optional extra write into guest memory before the run (bx_arm needs it).
struct RunResult {
    StorageMemory mem;
    Arm7Cpu cpu;
    Halt halt = Halt::kNone;
    RunResult() : cpu(mem) {}
};

static bool run_program(const char* name, std::unique_ptr<RunResult>* out,
                        uint32_t extra_at = 0, uint32_t extra_word = 0) {
    const ArmProg* p = find_prog(name);
    if (p == nullptr) return false;
    auto r = std::make_unique<RunResult>();
    const std::string img = std::string("physmem_") + name + ".bin";
    if (!r->mem.create(img, hakios::map::kPhysSize)) {
        std::printf("FAIL %s: cannot create guest image: %s\n", name, r->mem.last_error().c_str());
        ++g_failures;
        return false;
    }
    if (!r->mem.copy_in(p->org, p->words, p->n_words * sizeof(p->words[0]))) {
        std::printf("FAIL %s: cannot load program\n", name);
        ++g_failures;
        return false;
    }
    if (extra_word != 0 && !r->mem.copy_in(extra_at, &extra_word, 4)) {
        std::printf("FAIL %s: cannot place extra word\n", name);
        ++g_failures;
        return false;
    }
    r->cpu.reset(p->org);
    r->halt = r->cpu.run(10000);
    *out = std::move(r);
    return true;
}

int main(int argc, char** argv) {
    if (argc > 1 && ::chdir(argv[1]) != 0) {
        std::printf("FAIL: cannot chdir to %s\n", argv[1]);
        return 1;
    }

    std::unique_ptr<RunResult> r;

    // -- reset state ---------------------------------------------------------
    {
        StorageMemory mem;
        CHECK(mem.create("cpu_reset.bin", 1024 * 1024));
        Arm7Cpu cpu(mem);
        cpu.reset(0x1000);
        for (int i = 0; i < 15; ++i) CHECK(cpu.reg(i) == 0);
        CHECK(cpu.pc() == 0x1000);
        CHECK(cpu.cpsr() == 0x13);
        CHECK(cpu.info().halt == Halt::kNone);
        CHECK(cpu.info().executed == 0);
        CHECK(cpu.guest_size() == 1024 * 1024);
    }

    // -- flags_data_proc -------------------------------------------------------
    // r2/r3/r6/r8 hold CPSR snapshots taken with mrs right after each
    // flag-setting instruction (N=0x80000000 Z=0x40000000 C=0x20000000
    // V=0x10000000, mode 0x13).
    if (run_program("flags_data_proc", &r)) {
        CHECK(r->cpu.reg(0) == 0x1FF);
        CHECK(r->cpu.reg(1) == 0xFF000000);           // movs r1, r0, lsl #24
        CHECK(r->cpu.reg(2) == 0xA0000013u);          // N + C (shifter carry: bit 8 of 0x1FF)
        CHECK(r->cpu.reg(3) == 0x60000013u);          // Z + C from cmp r1, r1
        CHECK(r->cpu.reg(5) == 0x80000000);           // 0x7FFFFFFF + 1
        CHECK(r->cpu.reg(6) == 0x90000013u);          // N + V from signed overflow
        CHECK(r->cpu.reg(7) == 0);                    // subs r7, r7, #0
        CHECK(r->cpu.reg(8) == 0x60000013u);          // Z + C (no borrow)
        CHECK(r->cpu.reg(9) == 0xFFFFFFFF);           // mvn r9, #0
        CHECK(r->halt == Halt::kUnknownInstruction); // ran to the UDF
        CHECK(r->cpu.info().executed == 13);          // 12 instructions + the UDF
        CHECK(r->cpu.info().halt_pc == 0x1030);
    }

    // -- loop_bne ---------------------------------------------------------------
    if (run_program("loop_bne", &r)) {
        CHECK(r->cpu.reg(2) == 0);
        CHECK(r->cpu.reg(1) == 0);
        CHECK(r->halt == Halt::kUnknownInstruction);
        // mov + 5*(subs + bne) + mov + udf = 13
        CHECK(r->cpu.info().executed == 13);
    }

    // -- literal_pool (PC = insn + 8 on a PC-based load) -------------------------
    if (run_program("literal_pool", &r)) {
        const ArmProg* p = find_prog("literal_pool");
        CHECK(r->cpu.reg(0) == 0);
        // ldr r1, [pc, #-4] at 0x1004 loaded the word at 0x1008: the mov r3
        CHECK(r->cpu.reg(1) == p->words[2]);
        // ldr r2, [pc, #0] at 0x100C loaded 0x1014: the UDF sentinel
        CHECK(r->cpu.reg(2) == p->words[5]);
        CHECK(r->cpu.reg(2) == 0xE7F000F0u);
        CHECK(r->cpu.reg(4) == 0);
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- mem_bytes ---------------------------------------------------------------
    if (run_program("mem_bytes", &r)) {
        CHECK(r->cpu.reg(2) == 0x100);               // str/ldr word round trip
        CHECK(r->cpu.reg(4) == 0xAB);                // strb/ldrb
        CHECK(r->cpu.reg(5) == 0x1234);
        CHECK(r->cpu.reg(12) == 0x100);              // ldr r12, [r11, #-4]!
        CHECK(r->cpu.reg(6) == 0x1234);              // strh/ldrh imm offset 0x10
        CHECK(r->cpu.reg(8) == 0xFFFFFF80);          // ldrsb of 0x80
        CHECK(r->cpu.reg(9) == 0x0080);              // ldrh of 00 80
        CHECK(r->cpu.reg(10) == 0x00000080);        // ldrsh: positive halfword
        CHECK(r->cpu.reg(13) == 0x1234);             // ldrsh register offset: positive
        CHECK(r->cpu.reg(11) == 0x200100);          // writeback: pre-dec undone
        CHECK(r->halt == Halt::kUnknownInstruction);
        // the guest image holds what the CPU wrote
        CHECK(r->mem.read32(0x200000) == 0x100);
        CHECK(r->mem.read8(0x200004) == 0xAB);
        CHECK(r->mem.read16(0x200010) == 0x1234);
        CHECK(r->mem.read8(0x200014) == 0x80);
        CHECK(r->mem.read16(0x200018) == 0x1234);   // register-offset strh
    }

    // -- ldm_stm ----------------------------------------------------------------
    if (run_program("ldm_stm", &r)) {
        CHECK(r->cpu.reg(3) == 0x11);
        CHECK(r->cpu.reg(4) == 0x22);
        CHECK(r->cpu.reg(5) == 0x33);                // ldmdb from 0x20010C
        CHECK(r->cpu.reg(11) == 0x2000F4);          // ldmdb then stmdb writeback
        CHECK(r->cpu.reg(6) == 0x11);
        CHECK(r->cpu.reg(7) == 0x22);
        CHECK(r->cpu.reg(8) == 0x33);                // ldmia after stmdb
        CHECK(r->cpu.reg(13) == 0x22);              // ldmda from 0x200208
        CHECK(r->cpu.reg(14) == 0x33);
        CHECK(r->cpu.reg(12) == 0x200200);          // ldmda writeback
        CHECK(r->halt == Halt::kUnknownInstruction);
        CHECK(r->mem.read32(0x2000F4) == 0x11);
        CHECK(r->mem.read32(0x2000F8) == 0x22);
        CHECK(r->mem.read32(0x2000FC) == 0x33);
    }

    // -- mul_clz ----------------------------------------------------------------
    if (run_program("mul_clz", &r)) {
        CHECK(r->cpu.reg(2) == 700);                 // mul
        CHECK(r->cpu.reg(3) == 1700);                // mla
        CHECK(r->cpu.reg(4) == 700);                 // umull lo
        CHECK(r->cpu.reg(6) == 0);                   // umull hi
        CHECK(r->cpu.reg(8) == (uint32_t)-700);     // smull lo
        CHECK(r->cpu.reg(9) == 0xFFFFFFFF);          // smull hi
        CHECK(r->cpu.reg(11) == 23);                 // clz(0x100)
        CHECK(r->cpu.reg(13) == 32);                 // clz(0)
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- mrs_msr ----------------------------------------------------------------
    if (run_program("mrs_msr", &r)) {
        CHECK(r->cpu.reg(0) == 0x13);                // SVC mode after reset
        CHECK(r->cpu.reg(1) == 0x10);                // after msr cpsr_c, #0x10
        CHECK(r->cpu.reg(2) == 0x20000010);          // after msr cpsr_f, C
        CHECK(r->cpu.reg(3) == 0x20000013);          // after msr cpsr_c, r0
        CHECK(r->cpu.cpsr() == 0x20000013);
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- conds ------------------------------------------------------------------
    if (run_program("conds", &r)) {
        CHECK(r->cpu.reg(1) == 0);                   // beq taken
        CHECK(r->cpu.reg(2) == 0);                   // blt taken
        CHECK(r->cpu.reg(3) == 0x55);                // bhi not taken
        CHECK(r->cpu.reg(4) == 0);                   // bge taken
        CHECK(r->cpu.reg(6) == 0xAA);                // bvs not taken
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- vram_through_cpu: the CPU writes through to the storage image ----------
    if (run_program("vram_through_cpu", &r)) {
        CHECK(r->cpu.reg(1) == 0xDEADBEEF);
        CHECK(r->mem.read32(hakios::map::kVramBase) == 0xDEADBEEF);
        CHECK(r->mem.resident_pages(hakios::map::kVramBase, 4096) >= 1);  // it faulted in
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- rrx_raw ----------------------------------------------------------------
    if (run_program("rrx_raw", &r)) {
        CHECK(r->cpu.reg(6) == 0x80000000);          // RRX of 1 with C=1
        CHECK(r->cpu.flag_c());                      // new carry = bit 0 of r3 = 1
        CHECK(r->cpu.flag_n());
        CHECK(!r->cpu.flag_z());
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- reg_shift ----------------------------------------------------------------
    if (run_program("reg_shift", &r)) {
        CHECK(r->cpu.reg(1) == 0x180);               // 0x18 << 4
        CHECK(r->cpu.reg(2) == 1);                   // 0x18 >> 4
        CHECK(!r->cpu.flag_c());                     // bit 28 of 0x18 = 0
        // after clearing flags, LSL by r4 sets C from the shifter again
        CHECK(r->cpu.reg(5) == 0x180);
        CHECK(!r->cpu.flag_c());
        CHECK(r->halt == Halt::kUnknownInstruction);
    }

    // -- halts -----------------------------------------------------------------
    if (run_program("halt_svc", &r)) {
        CHECK(r->halt == Halt::kSupervisorCall);
        CHECK(r->cpu.info().halt_pc == 0x1000);
        CHECK(r->cpu.info().halt_insn == 0xEF000080);
        CHECK(r->cpu.info().executed == 1);
        // sticky: further steps return the same halt, nothing advances
        const uint64_t n = r->cpu.info().executed;
        CHECK(r->cpu.step() == Halt::kSupervisorCall);
        CHECK(r->cpu.info().executed == n);
    }
    if (run_program("halt_undef_coproc", &r)) {
        CHECK(r->halt == Halt::kUnknownInstruction);
        CHECK(r->cpu.info().halt_insn == 0xEE000010);
    }
    if (run_program("halt_media", &r)) {
        CHECK(r->halt == Halt::kUnknownInstruction);
        CHECK(r->cpu.info().halt_insn == 0xE6800130);
    }
    if (run_program("bx_thumb", &r)) {
        CHECK(r->halt == Halt::kThumbUnsupported);
        CHECK(r->cpu.reg(0) == 0x1001);
        CHECK(r->cpu.info().halt_pc == 0x1008);      // the bx itself
        CHECK(r->cpu.reg(1) == 0);                   // never ran
    }
    if (run_program("bx_arm", &r, 0x200000, 0xE7F000F0)) {
        CHECK(r->halt == Halt::kUnknownInstruction);  // the UDF at 0x200000
        CHECK(r->cpu.info().halt_pc == 0x200000);    // branch went through
        CHECK(r->cpu.reg(1) == 0);
    }

    // -- budget and state poking -------------------------------------------------
    {
        StorageMemory mem;
        CHECK(mem.create("cpu_budget.bin", 1024 * 1024));
        const ArmProg* p = find_prog("flags_data_proc");
        CHECK(p != nullptr);
        CHECK(mem.copy_in(p->org, p->words, p->n_words * 4));
        Arm7Cpu cpu(mem);
        cpu.reset(p->org);
        CHECK(cpu.run(2) == Halt::kNone);            // budget out
        CHECK(cpu.info().executed == 2);
        CHECK(cpu.pc() == p->org + 8);
        CHECK(cpu.run(100) == Halt::kUnknownInstruction);
        CHECK(cpu.info().executed == 13);
        // register poking round trip (the engine uses this)
        cpu.reset(p->org);
        cpu.set_reg(3, 0xCAFEBABE);
        CHECK(cpu.reg(3) == 0xCAFEBABE);
        cpu.set_pc(0x200000);
        CHECK(cpu.pc() == 0x200000);
        // fetch fault: PC far outside the guest image
        CHECK(cpu.step() == Halt::kFetchFault);
        CHECK(cpu.pc() == 0x200000);                 // sticky, no advance
        // halt names exist for every kind
        CHECK(std::strcmp(Arm7Cpu::halt_name(Halt::kThumbUnsupported), "thumb unsupported") == 0);
    }

    if (g_failures == 0) {
        std::printf("test_cpu: all checks passed\n");
        return 0;
    }
    std::printf("test_cpu: %d failure(s)\n", g_failures);
    return 1;
}
