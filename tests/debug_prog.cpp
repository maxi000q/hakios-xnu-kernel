// Scratch debug rig (not part of the test suite): run a single generated
// program and dump registers + memory so decode bugs are visible.
#include <cstdio>
#include <cstring>
#include "gen/arm_progs.h"
#include "hakios/arm7_cpu.h"
#include "hakios/guest_map.h"
#include "hakios/storage_memory.h"

using hakios::Arm7Cpu;
using hakios::StorageMemory;

int main(int argc, char** argv) {
    const char* name = argc > 1 ? argv[1] : "ldm_stm";
    const ArmProg* p = nullptr;
    for (size_t i = 0; i < kArmProgCount; ++i)
        if (std::strcmp(kArmProgs[i].name, name) == 0) p = &kArmProgs[i];
    if (!p) { std::printf("no program %s\n", name); return 1; }

    StorageMemory mem;
    if (!mem.create("debug_guest.bin", hakios::map::kPhysSize)) return 1;
    mem.copy_in(p->org, p->words, p->n_words * 4);
    Arm7Cpu cpu(mem);
    cpu.reset(p->org);
    const auto halt = cpu.run(10000);

    std::printf("program %s (%zu words)\n  asm: %s\n", p->name, p->n_words, p->asm_text);
    std::printf("halt=%s pc=0x%08x insn=0x%08x executed=%llu detail=%s\n",
                Arm7Cpu::halt_name(halt), cpu.info().halt_pc, cpu.info().halt_insn,
                (unsigned long long)cpu.info().executed, cpu.info().detail);
    for (int i = 0; i < 16; i += 4)
        std::printf("  r%-2d=0x%08x r%-2d=0x%08x r%-2d=0x%08x r%-2d=0x%08x\n", i, cpu.reg(i),
                    i + 1, cpu.reg(i + 1), i + 2, cpu.reg(i + 2), i + 3, cpu.reg(i + 3));
    std::printf("  cpsr=0x%08x N=%d Z=%d C=%d V=%d\n", cpu.cpsr(), cpu.flag_n(), cpu.flag_z(),
                cpu.flag_c(), cpu.flag_v());
    for (uint32_t a = 0x2000F0; a <= 0x200210; a += 4) {
        std::printf("  [%06x]=0x%08x%s\n", a, mem.read32(a), (a % 32 == 16) ? "" : "");
    }
    std::printf("  words:");
    for (size_t i = 0; i < p->n_words; ++i) std::printf(" %08X", p->words[i]);
    std::printf("\n");
    return 0;
}
