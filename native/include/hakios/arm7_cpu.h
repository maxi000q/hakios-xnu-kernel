#pragma once

// The emulated iPhone's processor -- stage 3: an ARMv7-A interpreter.
//
// iOS 6 (XNU 2107) runs on 32-bit ARMv7: no PAC, no PPL, short page tables.
// This class is the "CPU" of the emulated iPhone 4 -- a plain C++ decode
// loop over the storage-backed guest image from stage 2. It is pure code,
// touches no host hardware, and therefore runs on any Android device the
// app installs on (see devices/adreno-619/README.md); only speed varies
// with the phone.
//
// Coverage (ARM state, UAL encodings as assembled by keystone):
//   - all 15 conditions (+NV treated as AL)
//   - data processing: AND EOR SUB RSB ADD ADC SBC RSC TST TEQ CMP CMN
//     ORR MOV BIC MVN, immediate (rotated) and register operands with
//     LSL/LSR/ASR/ROR/RRX shifts (immediate and register-specified)
//   - LDR/STR/LDRB/STRB, immediate and register offsets, pre/post index,
//     writeback, PC-relative literal loads (base = insn + 8)
//   - LDRH/STRH/LDRSB/LDRSH (immediate and register offsets)
//   - LDM/STM: IA/IB/DA/DB, writeback
//   - MUL/MLA/UMULL/UMLAL/SMULL/SMLAL, CLZ
//   - MRS/MSR (CPSR, field masks), BX, B/BL, SVC
//
// Deliberately not yet, each halting with a distinct, reportable reason:
//   Thumb state (BX/BLX to a Thumb address, MSR setting the T bit) --
//   iOS 6 userland will need it, the kernel entry path does not;
//   LDRD/STRD, LDM^/STM^ (user-bank), MOVS pc (SPSR restore, needs
//   banked registers), coprocessor/VFP/NEON, media (parallel) ops.
// The engine (stage 6) surfaces these as "unsupported: <what>, at pc".
//
// Pipeline semantics: reads of r15 as an operand give insn+8; a store of
// r15 writes insn+12; writing r15 (including B/BL/BX and LDM/LDR of pc)
// branches. Memory accesses are unaligned-permissive via byte-wise loads,
// matching the Cortex-A generation the guest expects.

#include <cstdint>
#include <string>

#include "hakios/storage_memory.h"

namespace hakios {

class Arm7Cpu {
public:
    enum class Halt : uint8_t {
        kNone = 0,               // running, or the step budget ran out
        kUnknownInstruction,     // undecodable / unimplemented instruction
        kSupervisorCall,         // SVC executed; info.halt_insn holds the word
        kThumbUnsupported,       // switch to Thumb state requested
        kUnsupportedEncoding,    // decoded, deliberately not implemented yet
        kMisalignedPc,          // PC with low bits set in ARM state
        kFetchFault,             // instruction fetch outside the guest image
        kDataFault,              // data access outside the guest image
    };

    struct Info {
        Halt     halt = Halt::kNone;
        uint32_t halt_pc = 0;    // guest address of the halting instruction
        uint32_t halt_insn = 0;   // the instruction word itself
        uint64_t executed = 0;   // instructions retired, incl. the halting one
        char     detail[96] = {0};  // "LDRD", "LDM^ user-bank", ...
    };

    explicit Arm7Cpu(StorageMemory& mem) : mem_(mem) {}

    // Zero registers, CPSR = SVC mode with flags clear, halt cleared.
    void reset(uint32_t pc);

    // Execute exactly one instruction (or a skipped condition). The halt
    // is sticky: once set, step() returns it forever until reset().
    Halt step();

    // Run up to max_steps instructions; stops early on halt.
    Halt run(uint64_t max_steps);

    // -- state access, for the engine (stage 6) and the tests --------------
    uint32_t reg(int i) const { return r_[i]; }
    void     set_reg(int i, uint32_t v) { r_[i] = v; }
    uint32_t pc() const { return r_[15]; }
    void     set_pc(uint32_t v) { r_[15] = v; }
    uint32_t cpsr() const { return cpsr_; }
    void     set_cpsr(uint32_t v) { cpsr_ = v; }
    bool     flag_n() const { return (cpsr_ >> 31) & 1u; }
    bool     flag_z() const { return (cpsr_ >> 30) & 1u; }
    bool     flag_c() const { return (cpsr_ >> 29) & 1u; }
    bool     flag_v() const { return (cpsr_ >> 28) & 1u; }
    void     set_nz(uint32_t res) { set_n(res & 0x80000000u); set_z(res == 0); }

    const Info& info() const { return info_; }
    static const char* halt_name(Halt h);

    size_t guest_size() const { return mem_.size(); }

private:
    // instruction classes
    Halt execute(uint32_t pc_addr, uint32_t insn);
    Halt execute_dp(uint32_t pc_addr, uint32_t insn);
    Halt execute_ldst(uint32_t pc_addr, uint32_t insn, bool imm_form);
    Halt execute_ldst_ext(uint32_t pc_addr, uint32_t insn);
    Halt execute_ldm_stm(uint32_t pc_addr, uint32_t insn);
    Halt execute_multiply(uint32_t insn);
    Halt execute_msr(uint32_t insn, uint32_t value);

    // operand helpers
    uint32_t dp_operand2(uint32_t insn, uint32_t pc_addr, bool* carry_out);
    uint32_t imm_operand2(uint32_t insn, bool* carry_out);

    bool cond_pass(uint32_t cond) const;
    void set_n(bool v) { cpsr_ = (cpsr_ & ~0x80000000u) | (v ? 0x80000000u : 0); }
    void set_z(bool v) { cpsr_ = (cpsr_ & ~0x40000000u) | (v ? 0x40000000u : 0); }
    void set_c(bool v) { cpsr_ = (cpsr_ & ~0x20000000u) | (v ? 0x20000000u : 0); }
    void set_v(bool v) { cpsr_ = (cpsr_ & ~0x10000000u) | (v ? 0x10000000u : 0); }

    // guest memory, bounds-checked against the image, byte-wise (unaligned
    // permissive). Faults report false and the caller turns them into halts.
    bool rd32(uint32_t a, uint32_t* v) const;
    bool rd16(uint32_t a, uint32_t* v) const;
    bool rd8(uint32_t a, uint32_t* v) const;
    bool wr32(uint32_t a, uint32_t v);
    bool wr16(uint32_t a, uint32_t v);
    bool wr8(uint32_t a, uint32_t v);

    Halt halt(Arm7Cpu::Halt kind, uint32_t pc, uint32_t insn, const char* detail);
    Halt data_fault(uint32_t pc, uint32_t insn);

    StorageMemory& mem_;
    uint32_t r_[16];
    uint32_t cpsr_ = 0x13;  // SVC mode, flags clear
    Info info_;
};

}  // namespace hakios
