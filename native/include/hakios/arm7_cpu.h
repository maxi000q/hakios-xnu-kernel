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
//   - MRS/MSR (CPSR and SPSR, field masks), BX, B/BL, SVC
//   - MOVW/MOVT (ARMv7's constant builder), CPS (mode change honoured with
//     register banking, the interrupt masks stored), SETEND (a no-op: one
//     endianness)
//   - MRC/MCR p15, the A4's system coprocessor: identity registers with
//     Cortex-A8 model values, SCTLR/TTBR/DACR/CONTEXTIDR/TPIDR stored and
//     read back, cache/TLB maintenance as no-ops (see execute_cp15)
//   - LDRD/STRD, LDM^/STM^ (user-bank transfers) and MOVS pc / LDM {...,pc}^
//     (SPSR restore) over the banked register file: r13/r14 per mode, r8-r12
//     additionally banked in FIQ, one SPSR per privileged mode
//   - VFPv3-D16 (coprocessors 10/11) -- the unit Apple's 6.1.3 kernel probes
//     in vfp_init: FPSID/MVFR0/MVFR1/FPEXC/FPINST/FPINST2/FPSCR through
//     VMSR/VMRS, the f32/f64 arithmetic set (VADD..VDIV, VMLA family, VABS,
//     VNEG, VSQRT, VCMP), the conversions (integer, fixed-point f32,
//     f32<->f64, VCVTR), VMOV in every core<->VFP form, and VLDR/VSTR/
//     VLDM/VSTM over the coprocessor load/store space. Identity values are
//     the Cortex-A8's (the A4's core): FPSID 0x410330C0, MVFR0 0x11110222,
//     MVFR1 0x00011111. FPEXC.EN is stored and reported but does not gate
//     execution -- an interpreter has no VFP trap to take, and the kernel
//     probes by reading FPSID/MVFR, not by trapping.
//   - DSB/DMB/ISB barriers (no-ops in a serial interpreter), PLD/PLI/PLDW
//     prefetch hints (no-ops), NOP/YIELD/WFE/SEV; WFI is a clean halt
//
// Deliberately not yet, each halting with a distinct, reportable reason:
//   Thumb state (BX/BLX to a Thumb address, MSR setting the T bit) --
//   iOS 6 userland will need it, the kernel entry path does not;
//   NEON (the vector half of cp10/11) and the f64 fixed-point VCVT;
//   media (parallel) ops.
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
        kWaitForInterrupt,       // WFI in a machine with no interrupt source:
                                 // the wait would be forever, so it is a halt
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
    void reset(uint32_t pc, uint32_t sp = 0);

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

    // -- VFP state, for the engine and the tests --------------------------------
    // Registers are raw bits (s0..s31 alias the d0..d15 words: s(2n) is the
    // low half of d(n), s(2n+1) the high half, little-endian like the guest).
    uint32_t vfp_s(int i) const;
    void     set_vfp_s(int i, uint32_t bits);
    uint64_t vfp_d(int i) const;
    void     set_vfp_d(int i, uint64_t bits);
    uint32_t vfp_fpscr() const { return vfpscr_; }
    void     set_vfp_fpscr(uint32_t v) { vfpscr_ = v; }
    uint32_t vfp_fpexc() const { return fpexc_; }
    uint32_t vfp_sysid(int reg) const;   // 0 fpsid, 6 mvfr1, 7 mvfr0

    // The CPSR mode field, with banking (usr/sys share index 0).
    uint32_t mode() const { return cpsr_ & 0x1Fu; }

    size_t guest_size() const { return mem_.size(); }

private:
    // instruction classes
    Halt execute(uint32_t pc_addr, uint32_t insn);
    Halt execute_dp(uint32_t pc_addr, uint32_t insn);
    Halt execute_ldst(uint32_t pc_addr, uint32_t insn, bool imm_form);
    Halt execute_ldst_ext(uint32_t pc_addr, uint32_t insn);
    Halt execute_ldm_stm(uint32_t pc_addr, uint32_t insn);
    Halt execute_multiply(uint32_t insn);
    Halt execute_msr(uint32_t insn, uint32_t value, bool to_spsr = false);
    Halt execute_cp15(uint32_t pc_addr, uint32_t insn);
    Halt execute_cps(uint32_t pc_addr, uint32_t insn);

    // -- the VFP (coprocessors 10/11) ------------------------------------------
    Halt execute_vfp_dp(uint32_t pc_addr, uint32_t insn);        // CDP space
    Halt execute_vfp_transfer(uint32_t pc_addr, uint32_t insn);  // MRC/MCR space
    Halt execute_vfp_ldst(uint32_t pc_addr, uint32_t insn);      // LDC/STC space
    Halt execute_vfp_move2(uint32_t pc_addr, uint32_t insn);     // MCRR/MRRC space
    void vfp_compare_f32(uint32_t a, uint32_t b);   // VCMP: FPSCR N/Z/C/V
    void vfp_compare_f64(uint64_t a, uint64_t b);

    // -- banked register file ----------------------------------------------------
    // Index by mode: 0 usr/sys, 1 fiq, 2 irq, 3 svc, 4 mon, 5 abt, 6 und.
    static int mode_index(uint32_t mode);          // -1 when not a mode
    void switch_mode(uint32_t from, uint32_t to);  // swap the banked views
    uint32_t read_user_reg(int i) const;           // LDM^/STM^ view
    void write_user_reg(int i, uint32_t v);
    Halt restore_spsr(uint32_t pc_addr, uint32_t insn);  // MOVS pc / LDM {pc}^

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
    // The banked register file: r13/r14 per mode group, r8-r12 of the
    // usr/sys bank (FIQ replaces them), and one SPSR per privileged mode.
    uint32_t bank_r13_[7] = {};
    uint32_t bank_r14_[7] = {};
    uint32_t bank_usr_r8_12_[5] = {};   // the usr/sys bank of r8-r12 (FIQ hides it)
    uint32_t bank_fiq_r8_12_[5] = {};
    uint32_t bank_spsr_[7] = {};
    // The VFP: d0-d15 as raw bits (s0-s31 alias them), the FPSCR, and the
    // system registers XNU's vfp_init reads. FPSID/MVFR are constants of
    // the modelled Cortex-A8; FPEXC/FPINST/FPINST2 are stored.
    uint64_t d_[16] = {};
    uint32_t vfpscr_ = 0;
    uint32_t fpexc_ = 0;
    uint32_t fpinst_ = 0;
    uint32_t fpinst2_ = 0;
    // CP15's storage: [CRn][CRm][opc2], opc1 folded (the boot path uses
    // opc1 == 0 for control and opc1 == 1 for cache geometry, which reads
    // as zeros here -- an interpreter has no caches to describe).
    uint32_t cp15_[16][16][8] = {};
    Info info_;
};

}  // namespace hakios
