// The ARMv7-A interpreter -- implementation. Stage 3.
//
// The decode below was written against encodings MEASURED from the keystone
// assembler (scripts/gen_arm_tests.py prints and pins them), not from memory:
// multiply Rd lives at bits 19-16, MSR sits in the TEQ slot, ldrsb/ldrsh in
// the 0xD/0xF nibbles of the extension space. Where an encoding is excluded
// from a broader group (BX vs data-processing, MRS vs TST-with-S=0) the
// patterns are checked before the generic decode, mirroring the A4.1
// exclusion rules of the ARMv7 manual.

#include "hakios/arm7_cpu.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "hakios/platform.h"

namespace hakios {

namespace {

inline uint32_t bits(uint32_t v, int hi, int lo) {
    return (v >> lo) & ((1u << (hi - lo + 1)) - 1u);
}
inline bool bit(uint32_t v, int b) { return (v >> b) & 1u; }

inline int popcount16(uint16_t v) {
    int n = 0;
    while (v) { v &= (uint16_t)(v - 1); ++n; }  // Brian Kernighan: clear lowest set bit
    return n;
}

inline int clz32(uint32_t v) {
    if (v == 0) return 32;
    int n = 0;
    while ((v & 0x80000000u) == 0) { v <<= 1; ++n; }
    return n;
}

// -- shifter ---------------------------------------------------------------
// LSL/LSR/ASR/ROR by an immediate amount, with the imm5==0 special cases
// (LSL #0 = plain, LSR/ASR #0 = by 32, ROR #0 = RRX with the C-in rotate).
uint32_t shift_imm(uint32_t val, uint32_t type, uint32_t imm5, bool c_in, bool* carry_out) {
    switch (type) {
    case 0:  // LSL
        if (imm5 == 0) { *carry_out = c_in; return val; }
        *carry_out = (val >> (32 - imm5)) & 1u;
        return val << imm5;
    case 1: {  // LSR
        const uint32_t amt = imm5 == 0 ? 32 : imm5;
        *carry_out = amt == 32 ? (val >> 31) & 1u : (val >> (amt - 1)) & 1u;
        return amt == 32 ? 0 : val >> amt;
    }
    case 2: {  // ASR
        const uint32_t amt = imm5 == 0 ? 32 : imm5;
        const bool top = val & 0x80000000u;
        *carry_out = amt == 32 ? top : (val >> (amt - 1)) & 1u;
        if (amt == 32) return top ? 0xFFFFFFFFu : 0;
        return top ? (uint32_t)((int32_t)val >> amt) : val >> amt;
    }
    default: {  // ROR, and RRX when imm5 == 0
        if (imm5 == 0) {  // RRX: rotate right one through carry
            *carry_out = val & 1u;
            return (c_in ? 0x80000000u : 0u) | (val >> 1);
        }
        *carry_out = (val >> (imm5 - 1)) & 1u;
        return (val >> imm5) | (val << (32 - imm5));
    }
    }
}

// Shift by a register amount (bits 11-8 = Rs), full ARMv7 rules for
// amounts >= 32. Amount 0 performs nothing and carries nothing over.
uint32_t shift_reg(uint32_t val, uint32_t type, uint32_t amount, bool c_in, bool* carry_out) {
    if (amount == 0) { *carry_out = c_in; return val; }
    switch (type) {
    case 0:  // LSL
        if (amount < 32) { *carry_out = (val >> (32 - amount)) & 1u; return val << amount; }
        *carry_out = amount == 32 ? (val & 1u) : 0u;
        return 0;
    case 1:  // LSR
        if (amount < 32) { *carry_out = (val >> (amount - 1)) & 1u; return val >> amount; }
        *carry_out = amount == 32 ? (val >> 31) & 1u : 0u;
        return 0;
    case 2: {  // ASR
        const bool top = val & 0x80000000u;
        if (amount < 32) {
            *carry_out = (val >> (amount - 1)) & 1u;
            return (uint32_t)((int32_t)val >> amount);
        }
        *carry_out = top;
        return top ? 0xFFFFFFFFu : 0;
    }
    default: {  // ROR
        const uint32_t r = amount % 32;
        if (r == 0) { *carry_out = (val >> 31) & 1u; return val; }
        *carry_out = (val >> (r - 1)) & 1u;
        return (val >> r) | (val << (32 - r));
    }
    }
}

}  // namespace

const char* Arm7Cpu::halt_name(Halt h) {
    switch (h) {
    case Halt::kNone: return "none";
    case Halt::kUnknownInstruction: return "unknown instruction";
    case Halt::kSupervisorCall: return "supervisor call";
    case Halt::kThumbUnsupported: return "thumb unsupported";
    case Halt::kUnsupportedEncoding: return "unsupported encoding";
    case Halt::kMisalignedPc: return "misaligned pc";
    case Halt::kFetchFault: return "fetch fault";
    case Halt::kDataFault: return "data fault";
    case Halt::kWaitForInterrupt: return "wait for interrupt";
    }
    return "?";
}

void Arm7Cpu::reset(uint32_t pc, uint32_t sp) {
    std::memset(r_, 0, sizeof r_);
    r_[13] = sp;
    r_[15] = pc;
    cpsr_ = 0x13;  // SVC mode, flags clear, ARM state
    std::memset(bank_r13_, 0, sizeof bank_r13_);
    std::memset(bank_r14_, 0, sizeof bank_r14_);
    std::memset(bank_usr_r8_12_, 0, sizeof bank_usr_r8_12_);
    std::memset(bank_fiq_r8_12_, 0, sizeof bank_fiq_r8_12_);
    std::memset(bank_spsr_, 0, sizeof bank_spsr_);
    std::memset(d_, 0, sizeof d_);
    vfpscr_ = 0;
    fpexc_ = 0;
    fpinst_ = 0;
    fpinst2_ = 0;
    info_ = Info{};
}

Arm7Cpu::Halt Arm7Cpu::halt(Halt kind, uint32_t pc, uint32_t insn, const char* detail) {
    info_.halt = kind;
    info_.halt_pc = pc;
    info_.halt_insn = insn;
    if (detail != nullptr) {
        std::snprintf(info_.detail, sizeof info_.detail, "%s", detail);
    }
    return kind;
}

Arm7Cpu::Halt Arm7Cpu::data_fault(uint32_t pc, uint32_t insn) {
    return halt(Halt::kDataFault, pc, insn, "data access outside the guest image");
}

// -- guest memory ------------------------------------------------------------

bool Arm7Cpu::rd32(uint32_t a, uint32_t* v) const {
    if ((uint64_t)a + 4 > mem_.size()) return false;
    *v = load_u32(mem_.base() + a);
    return true;
}
bool Arm7Cpu::rd16(uint32_t a, uint32_t* v) const {
    if ((uint64_t)a + 2 > mem_.size()) return false;
    *v = load_u16(mem_.base() + a);
    return true;
}
bool Arm7Cpu::rd8(uint32_t a, uint32_t* v) const {
    if ((uint64_t)a + 1 > mem_.size()) return false;
    *v = mem_.base()[a];
    return true;
}
bool Arm7Cpu::wr32(uint32_t a, uint32_t v) {
    if ((uint64_t)a + 4 > mem_.size()) return false;
    store_u32(mem_.base() + a, v);
    return true;
}
bool Arm7Cpu::wr16(uint32_t a, uint32_t v) {
    if ((uint64_t)a + 2 > mem_.size()) return false;
    store_u16(mem_.base() + a, (uint16_t)v);
    return true;
}
bool Arm7Cpu::wr8(uint32_t a, uint32_t v) {
    if ((uint64_t)a + 1 > mem_.size()) return false;
    mem_.base()[a] = (uint8_t)v;
    return true;
}

// -- conditions ----------------------------------------------------------------

bool Arm7Cpu::cond_pass(uint32_t cond) const {
    const bool n = flag_n(), z = flag_z(), c = flag_c(), v = flag_v();
    switch (cond) {
    case 0x0: return z;   case 0x1: return !z;
    case 0x2: return c;   case 0x3: return !c;
    case 0x4: return n;   case 0x5: return !n;
    case 0x6: return v;   case 0x7: return !v;
    case 0x8: return c && !z;  case 0x9: return !c || z;
    case 0xA: return n == v;   case 0xB: return n != v;
    case 0xC: return !z && n == v;  case 0xD: return z || n != v;
    default: return true;  // 0xE AL; 0xF NV treated as AL (permissive)
    }
}

// -- operands -----------------------------------------------------------------

uint32_t Arm7Cpu::imm_operand2(uint32_t insn, bool* carry_out) {
    const uint32_t rot = bits(insn, 11, 8);
    const uint32_t v8 = bits(insn, 7, 0);
    uint32_t v;
    if (rot == 0) { v = v8; *carry_out = flag_c(); }
    else {
        const uint32_t r = 2 * rot;
        v = (v8 >> r) | (v8 << (32 - r));
        *carry_out = (v >> 31) & 1u;
    }
    return v;
}

uint32_t Arm7Cpu::dp_operand2(uint32_t insn, uint32_t pc_addr, bool* carry_out) {
    const uint32_t rm = bits(insn, 3, 0);
    const uint32_t val = rm == 15 ? pc_addr + 8 : r_[rm];
    const uint32_t type = bits(insn, 6, 5);
    if (bit(insn, 4)) {  // shifted by a register (bits 11-8 = Rs)
        const uint32_t amount = r_[bits(insn, 11, 8)] & 0xFF;
        return shift_reg(val, type, amount, flag_c(), carry_out);
    }
    return shift_imm(val, type, bits(insn, 11, 7), flag_c(), carry_out);
}

// -- instruction classes ---------------------------------------------------------

Arm7Cpu::Halt Arm7Cpu::execute_dp(uint32_t pc_addr, uint32_t insn) {
    const uint32_t opcode = bits(insn, 24, 21);
    const bool s = bit(insn, 20);
    const uint32_t rn = bits(insn, 19, 16);
    const uint32_t rd = bits(insn, 15, 12);

    bool carry = flag_c();
    const uint32_t op2 = bit(insn, 25) ? imm_operand2(insn, &carry) : dp_operand2(insn, pc_addr, &carry);
    const uint32_t a = rn == 15 ? pc_addr + 8 : r_[rn];

    uint32_t res = 0;
    uint32_t alu_c = 0;
    uint32_t alu_v = 0;
    bool writes = true;

    switch (opcode) {
    case 0x0: res = a & op2; break;                                       // AND
    case 0x1: res = a ^ op2; break;                                       // EOR
    case 0x2:  // SUB
        res = a - op2;
        alu_c = a >= op2;
        alu_v = (((a ^ op2) & (a ^ res)) >> 31) & 1u;
        break;
    case 0x3:  // RSB
        res = op2 - a;
        alu_c = op2 >= a;
        alu_v = (((op2 ^ a) & (op2 ^ res)) >> 31) & 1u;
        break;
    case 0x4: {  // ADD
        const uint64_t s64 = (uint64_t)a + op2;
        res = (uint32_t)s64; alu_c = (uint32_t)(s64 >> 32);
        alu_v = ((~(a ^ op2) & (a ^ res)) >> 31) & 1u;
        break;
    }
    case 0x5: {  // ADC
        const uint64_t s64 = (uint64_t)a + op2 + (flag_c() ? 1u : 0u);
        res = (uint32_t)s64; alu_c = (uint32_t)(s64 >> 32);
        alu_v = ((~(a ^ op2) & (a ^ res)) >> 31) & 1u;
        break;
    }
    case 0x6: {  // SBC: a - op2 - (1-C) == a + ~op2 + C
        const uint64_t s64 = (uint64_t)a + ~op2 + (flag_c() ? 1u : 0u);
        res = (uint32_t)s64; alu_c = (uint32_t)(s64 >> 32);
        alu_v = (((a ^ op2) & (a ^ res)) >> 31) & 1u;
        break;
    }
    case 0x7: {  // RSC: op2 - a - (1-C) == op2 + ~a + C
        const uint64_t s64 = (uint64_t)op2 + ~a + (flag_c() ? 1u : 0u);
        res = (uint32_t)s64; alu_c = (uint32_t)(s64 >> 32);
        alu_v = (((op2 ^ a) & (op2 ^ res)) >> 31) & 1u;
        break;
    }
    case 0x8: res = a & op2; writes = false; break;                      // TST
    case 0x9: res = a ^ op2; writes = false; break;                       // TEQ
    case 0xA:  // CMP
        res = a - op2;
        alu_c = a >= op2;
        alu_v = (((a ^ op2) & (a ^ res)) >> 31) & 1u;
        writes = false;
        break;
    case 0xB: {  // CMN
        const uint64_t s64 = (uint64_t)a + op2;
        res = (uint32_t)s64; alu_c = (uint32_t)(s64 >> 32);
        alu_v = ((~(a ^ op2) & (a ^ res)) >> 31) & 1u;
        writes = false;
        break;
    }
    case 0xC: res = a | op2; break;                                       // ORR
    case 0xD: res = op2; break;                                           // MOV
    case 0xE: res = a & ~op2; break;                                      // BIC
    default:   res = ~op2; break;                                         // MVN
    }

    if (s) {
        set_n(res & 0x80000000u);
        set_z(res == 0);
        const bool logical =
            opcode == 0x0 || opcode == 0x1 || opcode == 0x8 || opcode == 0x9 ||
            opcode == 0xC || opcode == 0xD || opcode == 0xE || opcode == 0xF;
        if (logical) set_c(carry);
        else { set_c(alu_c != 0); set_v(alu_v != 0); }
    }

    if (writes) {
        if (rd == 15) {
            if (s) {
                // MOVS pc / LDM-with-S: the SPSR restore, the exception return
                const Halt h = restore_spsr(pc_addr, insn);
                if (h != Halt::kNone) return h;
            }
            r_[15] = res;  // data-processing write to PC = branch
        } else {
            r_[rd] = res;
        }
    }
    return Halt::kNone;
}

Arm7Cpu::Halt Arm7Cpu::execute_ldst(uint32_t pc_addr, uint32_t insn, bool imm_form) {
    const bool p = bit(insn, 24), u = bit(insn, 23), b = bit(insn, 22),
               w = bit(insn, 21), l = bit(insn, 20);
    const uint32_t rn = bits(insn, 19, 16);
    const uint32_t rt = bits(insn, 15, 12);

    uint32_t off;
    if (imm_form) {
        off = bits(insn, 11, 0);
    } else {
        // register offset, immediate shift; bit4 == 0 in this form
        const uint32_t rm = bits(insn, 3, 0);
        const uint32_t val = rm == 15 ? pc_addr + 8 : r_[rm];
        bool carry = false;
        off = shift_imm(val, bits(insn, 6, 5), bits(insn, 11, 7), flag_c(), &carry);
    }

    const uint32_t base_v = rn == 15 ? pc_addr + 8 : r_[rn];
    const uint32_t addr = p ? (u ? base_v + off : base_v - off) : base_v;

    if (l) {
        uint32_t v = 0;
        const bool ok = b ? rd8(addr, &v) : rd32(addr, &v);
        if (!ok) return data_fault(pc_addr, insn);
        if (rt == 15) r_[15] = v & ~3u;  // LDR pc: branch (documented mask)
        else r_[rt] = v;
    } else {
        const uint32_t store_v = rt == 15 ? pc_addr + 12 : r_[rt];
        const bool ok = b ? wr8(addr, store_v) : wr32(addr, store_v);
        if (!ok) return data_fault(pc_addr, insn);
    }

    if (p ? w : true) {  // post-indexed always writes back
        if (rn == 15) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "load/store writeback to PC");
        r_[rn] = u ? base_v + off : base_v - off;
    }
    return Halt::kNone;
}

Arm7Cpu::Halt Arm7Cpu::execute_ldst_ext(uint32_t pc_addr, uint32_t insn) {
    // bits 7-4: 0xB = STRH/LDRH, 0xD = LDRSB (L=1) / LDRD (L=0), 0xF = LDRSH (L=1) / STRD (L=0)
    const uint32_t nib = bits(insn, 7, 4);
    const bool l = bit(insn, 20);
    const bool is_ldrd = (nib == 0xD && !l);
    const bool is_strd = (nib == 0xF && !l);

    if (!is_ldrd && !is_strd) {
    const uint32_t rn = bits(insn, 19, 16);
    const uint32_t rt = bits(insn, 15, 12);
    if (rt == 15) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "halfword load to PC");

    uint32_t off;
    // Immediate vs register offset in this space is decided by bit 22 (the I
    // bit), exactly as in the ordinary load-store space: set = the offset is
    // imm4H:imm4L (bits 11-8 and 3-0), clear = the offset register Rm is
    // bits 3-0 and bits 11-8 are SBZ. That bit is the whole story -- an
    // earlier version of this decoder replaced it with a "which one does the
    // nibble pattern mean" heuristic, and the stage-8 demo kernel's
    // `strh r2, [r9], #2` came out as `strh r2, [r9, r2]`, writing every
    // pixel of every row to an offset the kernel never asked for.
    if (bit(insn, 22)) {
        off = (bits(insn, 11, 8) << 4) | bits(insn, 3, 0);
    } else {
        const uint32_t rm = bits(insn, 3, 0);
        off = rm == 15 ? pc_addr + 8 : r_[rm];
    }

    const bool p = bit(insn, 24), u = bit(insn, 23), w = bit(insn, 21);
    const uint32_t base_v = rn == 15 ? pc_addr + 8 : r_[rn];
    const uint32_t addr = p ? (u ? base_v + off : base_v - off) : base_v;

    if (l) {
        uint32_t v = 0;
        if (nib == 0xB) {  // LDRH
            if (!rd16(addr, &v)) return data_fault(pc_addr, insn);
            r_[rt] = v;
        } else if (nib == 0xD) {  // LDRSB
            if (!rd8(addr, &v)) return data_fault(pc_addr, insn);
            r_[rt] = v & 0x80u ? v | 0xFFFFFF00u : v;
        } else {  // LDRSH
            if (!rd16(addr, &v)) return data_fault(pc_addr, insn);
            r_[rt] = v & 0x8000u ? v | 0xFFFF0000u : v;
        }
    } else {  // STRH
        if (!wr16(addr, r_[rt])) return data_fault(pc_addr, insn);
    }

    if (p ? w : true) {
        if (rn == 15) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "halfword writeback to PC");
        r_[rn] = u ? base_v + off : base_v - off;
    }
    return Halt::kNone;
    }

    // LDRD / STRD: an even register pair, eight bytes, the same offset
    // arithmetic as the halfword forms (bit 22 immediate vs register).
    const uint32_t rn = bits(insn, 19, 16);
    const uint32_t rt = bits(insn, 15, 12);
    if (rt & 1u || rt >= 14)
        return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                    "LDRD/STRD without an even register pair");
    uint32_t off;
    if (bit(insn, 22)) {
        off = (bits(insn, 11, 8) << 4) | bits(insn, 3, 0);
    } else {
        const uint32_t rm = bits(insn, 3, 0);
        off = rm == 15 ? pc_addr + 8 : r_[rm];
    }
    const bool p = bit(insn, 24), u = bit(insn, 23), w = bit(insn, 21);
    const uint32_t base_v = rn == 15 ? pc_addr + 8 : r_[rn];
    const uint32_t addr = p ? (u ? base_v + off : base_v - off) : base_v;

    if (is_ldrd) {
        if (!rd32(addr, &r_[rt]) || !rd32(addr + 4, &r_[rt + 1]))
            return data_fault(pc_addr, insn);
    } else {
        if (!wr32(addr, r_[rt]) || !wr32(addr + 4, r_[rt + 1]))
            return data_fault(pc_addr, insn);
    }

    if (p ? w : true) {
        if (rn == 15) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "LDRD/STRD writeback to PC");
        r_[rn] = u ? base_v + off : base_v - off;
    }
    return Halt::kNone;
}

Arm7Cpu::Halt Arm7Cpu::execute_ldm_stm(uint32_t pc_addr, uint32_t insn) {
    const bool p = bit(insn, 24), u = bit(insn, 23), s = bit(insn, 22),
               w = bit(insn, 21), l = bit(insn, 20);
    const uint16_t list = (uint16_t)(insn & 0xFFFFu);
    const uint32_t rn = bits(insn, 19, 16);

    if (list == 0) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "LDM/STM with empty register list");
    if (rn == 15) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "LDM/STM with PC base");
    if ((list >> rn) & 1u) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "LDM/STM with Rn in the list");

    const int count = popcount16(list);
    const uint32_t base_v = r_[rn];
    uint32_t addr = u ? (p ? base_v + 4 : base_v)
                      : (p ? base_v - 4 * count : base_v - 4 * (count - 1));

    if (w) r_[rn] = u ? base_v + 4 * count : base_v - 4 * count;

    if (s && !(l && ((list >> 15) & 1u))) {
        // LDM^ / STM^: the user-bank transfer -- every register named in the
        // list is read or written in the usr/sys bank, the current mode's
        // own view stays untouched (this is how the kernel saves and
        // restores a user thread's registers).
        for (int i = 0; i < 15; ++i) {
            if (!((list >> i) & 1u)) continue;
            if (l) {
                uint32_t v = 0;
                if (!rd32(addr, &v)) return data_fault(pc_addr, insn);
                write_user_reg(i, v);
            } else {
                if (!wr32(addr, read_user_reg(i))) return data_fault(pc_addr, insn);
            }
            addr += 4;
        }
        return Halt::kNone;
    }

    for (int i = 0; i < 16; ++i) {
        if (!((list >> i) & 1u)) continue;
        if (l) {
            uint32_t v = 0;
            if (!rd32(addr, &v)) return data_fault(pc_addr, insn);
            if (i == 15) {
                r_[15] = v & ~3u;  // LDM {pc}: branch (documented mask)
                if (s) {           // LDM {.., pc}^: the exception return
                    const Halt h = restore_spsr(pc_addr, insn);
                    if (h != Halt::kNone) return h;
                }
            } else if (s) {
                write_user_reg(i, v);
            } else {
                r_[i] = v;
            }
        } else {
            const uint32_t v = i == 15 ? pc_addr + 12 : r_[i];
            if (!wr32(addr, v)) return data_fault(pc_addr, insn);
        }
        addr += 4;
    }
    return Halt::kNone;
}

Arm7Cpu::Halt Arm7Cpu::execute_multiply(uint32_t insn) {
    const uint32_t op = bits(insn, 23, 20);
    const bool s = bit(insn, 20);
    const uint32_t rm = bits(insn, 3, 0);
    const uint32_t rs = bits(insn, 11, 8);

    if (op == 0x0 || op == 0x2) {  // MUL / MLA; Rd at bits 19-16, addend at 15-12
        const uint32_t rd = bits(insn, 19, 16);
        if (rd == 15) return halt(Halt::kUnsupportedEncoding, 0, insn, "multiply to PC");
        const uint32_t prod = r_[rm] * r_[rs];
        const uint32_t res = op == 0x2 ? prod + r_[bits(insn, 15, 12)] : prod;
        if (s) set_nz(res);  // C, V unchanged (unpredictable per the manual)
        r_[rd] = res;
        return Halt::kNone;
    }

    // long: UMULL 0x8, UMLAL 0xA, SMULL 0xC, SMLAL 0xE (bits 23-20)
    const uint32_t rdhi = bits(insn, 19, 16);
    const uint32_t rdlo = bits(insn, 15, 12);
    if (rdhi == rdlo || rdhi == 15 || rdlo == 15)
        return halt(Halt::kUnsupportedEncoding, 0, insn, "bad long-multiply register pair");
    const bool signed_ = (op & 0x4) != 0;
    const bool accumulate = (op & 0x2) != 0;
    uint64_t prod = signed_ ? (uint64_t)((int64_t)(int32_t)r_[rm] * (int64_t)(int32_t)r_[rs])
                            : (uint64_t)r_[rm] * (uint64_t)r_[rs];
    if (accumulate) prod += ((uint64_t)r_[rdhi] << 32) | r_[rdlo];
    const uint32_t lo = (uint32_t)prod;
    const uint32_t hi = (uint32_t)(prod >> 32);
    if (s) {
        set_n(hi & 0x80000000u);
        set_z((hi | lo) == 0);
    }
    r_[rdlo] = lo;
    r_[rdhi] = hi;
    return Halt::kNone;
}

Arm7Cpu::Halt Arm7Cpu::execute_msr(uint32_t insn, uint32_t value, bool to_spsr) {
    const uint32_t mask_nib = bits(insn, 19, 16);
    uint32_t mask = 0;
    if (mask_nib & 1u) mask |= 0x000000FFu;  // c: control
    if (mask_nib & 2u) mask |= 0x0000FF00u;  // x: extension
    if (mask_nib & 4u) mask |= 0x00FF0000u;  // s: status
    if (mask_nib & 8u) mask |= 0xFF000000u;  // f: flags
    if (to_spsr) {
        // The current privileged mode's SPSR; writing it never changes the
        // mode (the return does, from the SPSR's own copy).
        const int mi = mode_index(mode());
        if (mi <= 0)
            return halt(Halt::kUnsupportedEncoding, 0, insn,
                        "MSR spsr in a mode without one");
        bank_spsr_[mi] = (bank_spsr_[mi] & ~mask) | (value & mask);
        return Halt::kNone;
    }
    const uint32_t from = mode();
    cpsr_ = (cpsr_ & ~mask) | (value & mask);
    if (mask & 0x1Fu) {  // a control write may change the mode: bank the file
        if (mode_index(cpsr_ & 0x1Fu) < 0)
            return halt(Halt::kUnsupportedEncoding, 0, insn, "invalid CPSR mode");
        switch_mode(from, cpsr_ & 0x1Fu);
    }
    if (cpsr_ & 0x20u)  // T bit: Thumb state switch
        return halt(Halt::kThumbUnsupported, 0, insn, "MSR set the Thumb bit");
    return Halt::kNone;
}

// -- the A4's system coprocessor (CP15) ---------------------------------------
//
// XNU's first acts are CP15 acts: start.s invalidates caches, loads the
// translation-table base and the domain register, and turns the MMU on in
// SCTLR before its first C call. The model is the whole of what a boot
// needs and nothing more, every choice documented:
//
//   identity  MIDR carries ARM implementer, Cortex-A8 part (the A4's core,
//             S5L8930, a single CPU -- model revision digits); MPIDR is 0,
//             the one core there is; CTR describes 64-byte lines both
//             sides; CLIDR/CCSIDR report NO caches -- the truth for an
//             interpreter, which has no caches to maintain
//   control   SCTLR/ACTLR/TTBR0/TTBR1/DACR/CONTEXTIDR/TPIDR* are stored and
//             read back; the MMU enable bit is recorded, but the machine
//             stays flat VA == PA: XNU maps RAM and the devices 1:1 at
//             boot, which is exactly what the storage image already is
//   maintain  every c7 cache and c8 TLB op is a no-op (nothing to maintain)
//   faults    DFSR/IFSR/DFAR/IFAR are stored, for a panic path to print
Arm7Cpu::Halt Arm7Cpu::execute_cp15(uint32_t pc_addr, uint32_t insn) {
    const uint32_t crn  = bits(insn, 19, 16);
    const uint32_t rt   = bits(insn, 15, 12);
    const uint32_t opc1 = bits(insn, 23, 21);
    const uint32_t crm  = bits(insn, 3, 0);
    const uint32_t opc2 = bits(insn, 7, 5);

    if (bit(insn, 20)) {  // MRC: a read
        uint32_t v = cp15_[crn][crm][opc2];
        if (crn == 0 && crm == 0 && opc1 == 0) {
            if (opc2 == 0) v = 0x414FC091u;      // MIDR: ARM Cortex-A8 (model)
            if (opc2 == 1) v = 0x80E28002u;      // CTR: 64-byte lines, no caches
            if (opc2 == 5) v = 0x00000000u;      // MPIDR: the single core
        }
        if (crn == 1 && crm == 0 && opc2 == 0 && opc1 == 0 && cp15_[1][0][0] == 0)
            v = 0x00C50078u;  // SCTLR's architectural reset, before first write
        if (rt != 15) r_[rt] = v;   // to r15: unpredictable per the manual; skip
        return Halt::kNone;
    }

    // MCR: a write. Maintenance (c7, c8) changes nothing; the rest stores.
    if (crn != 7 && crn != 8)
        cp15_[crn][crm][opc2] = rt == 15 ? pc_addr + 8 : r_[rt];
    return Halt::kNone;
}

// -- the banked register file ---------------------------------------------------
//
// r13/r14 are banked per mode (usr and sys share one bank), r8-r12
// additionally in FIQ, and every privileged mode owns an SPSR. r_[] is
// always the current mode's view; switch_mode() swaps the parked views.
// LDM^/STM^ address the usr/sys bank without switching (read_user_reg /
// write_user_reg), and MOVS pc / LDM {.., pc}^ restore the CPSR from the
// current mode's SPSR (restore_spsr) -- the exception-return path XNU's
// kernel takes through every trap it handles.

int Arm7Cpu::mode_index(uint32_t mode) {
    switch (mode) {
    case 0x10: case 0x1F: return 0;  // usr and sys share the bank
    case 0x11: return 1;             // fiq
    case 0x12: return 2;             // irq
    case 0x13: return 3;             // svc
    case 0x16: return 4;             // mon
    case 0x17: return 5;             // abt
    case 0x1A: return 6;             // und
    default:   return -1;
    }
}

void Arm7Cpu::switch_mode(uint32_t from, uint32_t to) {
    const int fi = mode_index(from), ti = mode_index(to);
    if (fi < 0 || ti < 0 || fi == ti) return;
    bank_r13_[fi] = r_[13];
    bank_r14_[fi] = r_[14];
    if (from == 0x11) {  // leaving FIQ: park its r8-r12, restore the shared set
        for (int i = 0; i < 5; ++i) bank_fiq_r8_12_[i] = r_[8 + i];
        for (int i = 0; i < 5; ++i) r_[8 + i] = bank_usr_r8_12_[i];
    } else {             // in a shared mode: remember r8-r12 as the usr bank
        for (int i = 0; i < 5; ++i) bank_usr_r8_12_[i] = r_[8 + i];
    }
    r_[13] = bank_r13_[ti];
    r_[14] = bank_r14_[ti];
    if (to == 0x11) {    // entering FIQ: park the shared set, load FIQ's own
        for (int i = 0; i < 5; ++i) r_[8 + i] = bank_fiq_r8_12_[i];
    }
}

uint32_t Arm7Cpu::read_user_reg(int i) const {
    if (i >= 0 && i <= 7) return r_[i];
    if (i >= 8 && i <= 12) return (mode() == 0x11) ? bank_usr_r8_12_[i - 8] : r_[i];
    if (i == 13) return bank_r13_[0];
    if (i == 14) return bank_r14_[0];
    return 0;  // r15 is never in the user-bank list
}

void Arm7Cpu::write_user_reg(int i, uint32_t v) {
    if (i >= 0 && i <= 7) { r_[i] = v; return; }
    if (i >= 8 && i <= 12) {
        if (mode() == 0x11) bank_usr_r8_12_[i - 8] = v;
        else r_[i] = v;  // the current view IS the usr bank outside FIQ
        return;
    }
    if (i == 13) { bank_r13_[0] = v; if (mode_index(mode()) == 0) r_[13] = v; return; }
    if (i == 14) { bank_r14_[0] = v; if (mode_index(mode()) == 0) r_[14] = v; return; }
}

Arm7Cpu::Halt Arm7Cpu::restore_spsr(uint32_t pc_addr, uint32_t insn) {
    const int mi = mode_index(mode());
    if (mi <= 0)
        return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                    "SPSR restore in a mode without one");
    const uint32_t saved = bank_spsr_[mi];
    const uint32_t to = saved & 0x1Fu;
    if (mode_index(to) < 0)
        return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                    "invalid mode in the restored SPSR");
    switch_mode(mode(), to);
    cpsr_ = saved;
    if (cpsr_ & 0x20u)
        return halt(Halt::kThumbUnsupported, pc_addr, insn,
                    "SPSR restore to Thumb state");
    return Halt::kNone;
}

// -- the VFP (coprocessors 10 and 11) ---------------------------------------------
//
// The unit Apple's 6.1.3 kernel probes in vfp_init. Every encoding below
// was measured off the keystone assembler, the same discipline as the
// integer core, and the surprises are worth recording:
//   - a single-precision register index is V:N with N the LOW bit
//     (Sd = 2*Vd + D), while a double-precision index is D:Vd with D HIGH
//     (Dd = 16*D + Vd) -- the same bits, opposite roles;
//   - in the unary space (op3 6/7) bit 7 and bit 22 stop being register
//     bits and become opcode (the S/U and round selectors of VCVT), so the
//     destination/source encodings follow each instruction's own types;
//   - the fixed-point fraction of VCVT rides bits 5:0 as 32 - imm5 with
//     the M bit low, exactly like an s-register index;
//   - the register-pair VMOV (MCRR/MRRC) carries Dd in bits 5:0 the same
//     way, because bits 22 and 7 belong to its opc1.
// Identity values are the Cortex-A8's -- the core the A4 is built on:
// FPSID 0x410330C0 (ARM, VFPv3), MVFR0 0x11110222, MVFR1 0x00011111.

namespace {

inline float bits_f32(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
inline uint32_t f32_bits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
inline double bits_f64(uint64_t v) { double f; std::memcpy(&f, &v, 8); return f; }
inline uint64_t f64_bits(double f) { uint64_t v; std::memcpy(&v, &f, 8); return v; }

inline bool f32_isnan(uint32_t v) {
    return ((v >> 23) & 0xFFu) == 0xFFu && (v & 0x7FFFFFu) != 0;
}
inline bool f64_isnan(uint64_t v) {
    return ((v >> 52) & 0x7FFull) == 0x7FFull && (v & 0xFFFFFFFFFFFFFull) != 0;
}

}  // namespace

uint32_t Arm7Cpu::vfp_s(int i) const {
    if (i < 0 || i > 31) return 0;
    return (i & 1) ? uint32_t(d_[i >> 1] >> 32) : uint32_t(d_[i >> 1]);
}

void Arm7Cpu::set_vfp_s(int i, uint32_t b) {
    if (i < 0 || i > 31) return;
    if (i & 1) d_[i >> 1] = (d_[i >> 1] & 0xFFFFFFFFull) | (uint64_t(b) << 32);
    else       d_[i >> 1] = (d_[i >> 1] & ~0xFFFFFFFFull) | b;
}

uint64_t Arm7Cpu::vfp_d(int i) const { return (i >= 0 && i <= 15) ? d_[i] : 0; }
void Arm7Cpu::set_vfp_d(int i, uint64_t b) { if (i >= 0 && i <= 15) d_[i] = b; }

uint32_t Arm7Cpu::vfp_sysid(int reg) const {
    switch (reg) {
    case 0: return 0x410330C0u;  // FPSID: implementer ARM, architecture VFPv3
    case 7: return 0x11110222u;  // MVFR0
    case 6: return 0x00011111u;  // MVFR1
    default: return 0;
    }
}

// One VFP comparison per width, shared by VCMP's shapes. FPSCR's N/Z/C/V
// carry the result: unordered sets C+V, equal sets Z+C, less sets N,
// greater C. (-0 and +0 compare equal, as they must.)
void Arm7Cpu::vfp_compare_f32(uint32_t a, uint32_t b) {
    uint32_t n = 0, z = 0, c = 0, v = 0;
    if (f32_isnan(a) || f32_isnan(b)) { c = 1; v = 1; }
    else if (bits_f32(a) == bits_f32(b)) { z = 1; c = 1; }
    else if (bits_f32(a) < bits_f32(b)) n = 1;
    else c = 1;
    vfpscr_ = (vfpscr_ & ~0xF0000000u) | (n << 31) | (z << 30) | (c << 29) | (v << 28);
}

void Arm7Cpu::vfp_compare_f64(uint64_t a, uint64_t b) {
    uint32_t n = 0, z = 0, c = 0, v = 0;
    if (f64_isnan(a) || f64_isnan(b)) { c = 1; v = 1; }
    else if (bits_f64(a) == bits_f64(b)) { z = 1; c = 1; }
    else if (bits_f64(a) < bits_f64(b)) n = 1;
    else c = 1;
    vfpscr_ = (vfpscr_ & ~0xF0000000u) | (n << 31) | (z << 30) | (c << 29) | (v << 28);
}

Arm7Cpu::Halt Arm7Cpu::execute_vfp_dp(uint32_t pc_addr, uint32_t insn) {
    // The CDP space: bits 27-24 == 1110, bit 4 == 0, coprocessor 10 or 11.
    const bool sz = bit(insn, 8);           // 0: f32, 1: f64
    const uint32_t vd = bits(insn, 15, 12), vn = bits(insn, 19, 16), vm = bits(insn, 3, 0);
    const bool d_bit = bit(insn, 22), n_bit = bit(insn, 7), m_bit = bit(insn, 5);
    const bool op = bit(insn, 6);           // add/subtract selector of the pairs
    // op3 is bits 23/21/20 -- bit 22 is the D field of the destination and
    // must not take part in the opcode (a high-numbered register would
    // otherwise decode as a different instruction).
    const uint32_t op3 = (bit(insn, 23) << 2) | (bit(insn, 21) << 1) | bit(insn, 20);

    // Both encodings of the destination and the source, because the
    // conversion forms mix types (the f32<->f64 VCVT reads a d-register
    // into an s-register and back).
    const uint32_t sd_s = 2 * vd + (d_bit ? 1u : 0u);
    const uint32_t sd_d = (d_bit ? 16u : 0u) + vd;
    const uint32_t sn = sz ? ((n_bit ? 16u : 0u) + vn) : (2 * vn + (n_bit ? 1u : 0u));
    const uint32_t sm_s = 2 * vm + (m_bit ? 1u : 0u);
    const uint32_t sm_d = (m_bit ? 16u : 0u) + vm;

    if (op3 <= 0x4) {  // the binary arithmetic set
        if (!sz) {
            const float a = bits_f32(vfp_s(sn)), b = bits_f32(vfp_s(sm_s));
            const float acc = bits_f32(vfp_s(sd_s));
            float r = 0;
            switch (op3) {
            case 0x0: {  // VMLA / VMLS: the product rounds to single first
                const float prod = bits_f32(f32_bits((float)((double)a * (double)b)));
                r = op ? (acc - prod) : (acc + prod);
                break;
            }
            case 0x1: {  // VNMLS / VNMLA
                const float prod = bits_f32(f32_bits((float)((double)a * (double)b)));
                r = op ? -(acc + prod) : (prod - acc);
                break;
            }
            case 0x2: r = a * b; if (n_bit) r = -r; break;  // VMUL / VNMUL
            case 0x3: r = op ? (a - b) : (a + b); break;    // VSUB / VADD
            default: {  // VDIV; IEEE division gives VFP's inf/NaN answers
                if (b == 0.0f) vfpscr_ |= 0x00000002u;      // DZC
                if (f32_isnan(f32_bits(a / b)) && !f32_isnan(f32_bits(a)) && !f32_isnan(f32_bits(b)))
                    vfpscr_ |= 0x00000001u;                 // IOC (0/0)
                r = a / b;
                break;
            }
            }
            set_vfp_s(sd_s, f32_bits(r));
        } else {
            const double a = bits_f64(vfp_d(sn)), b = bits_f64(vfp_d(sm_d));
            const double acc = bits_f64(vfp_d(sd_d));
            double r = 0;
            switch (op3) {
            case 0x0: r = op ? (acc - a * b) : (acc + a * b); break;
            case 0x1: r = op ? -(acc + a * b) : (a * b - acc); break;
            case 0x2: r = a * b; if (n_bit) r = -r; break;
            case 0x3: r = op ? (a - b) : (a + b); break;
            default:
                if (b == 0.0) vfpscr_ |= 0x00000002u;
                r = a / b;
                break;
            }
            set_vfp_d(sd_d, f64_bits(r));
        }
        return Halt::kNone;
    }

    // op3 == 7: the unary space -- every one of the measured unary and
    // convert forms has bits 23/21/20 set (bit 22 is the D field of the
    // destination throughout; the fixed-point VCVT, VABS and the rest are
    // told apart by Vn and bits 7:6, where bit 7 stops being a register
    // index and becomes the S/U or round selector, and bit 6 is the op
    // marker -- clear only for the VMOV immediate, whose imm4H rides Vn.
    const uint32_t q = (bit(insn, 7) << 1) | bit(insn, 6);

    if (q == 0x0) {  // VMOV immediate: imm8 = imm4H(Vn) : imm4L(Vm)
        const uint32_t imm8 = (vn << 4) | vm;
        const uint32_t cd = (imm8 >> 4) & 3u;
        const uint32_t b = (imm8 >> 6) & 1u;
        if (!sz) {
            // exponent = 0x80 + cd - 4b (biased), mantissa = efgh at bit 19
            const uint32_t exp = 0x80u + cd - (b << 2);
            set_vfp_s(sd_s, ((imm8 >> 7) << 31) | (exp << 23) | ((imm8 & 0xFu) << 19));
        } else {
            const uint32_t exp = 0x400u + cd - (b << 2);
            set_vfp_d(sd_d, ((uint64_t)(imm8 >> 7) << 63) |
                            ((uint64_t)exp << 52) | ((uint64_t)(imm8 & 0xFu) << 48));
        }
        return Halt::kNone;
    }

    if (vn == 0xA || vn == 0xB || vn == 0xE || vn == 0xF) {
        // VCVT between floating-point and fixed-point, f32 forms. Two
        // things the encodings themselves say: the operation is a
        // read-modify-write of Sd (the source register the syntax names is
        // simply not encoded -- every source operand assembles to the same
        // word), and the fraction width is 32 - imm5, imm5 = (Vm << 1) | M
        // -- the M bit LOW, exactly like an s-register index.
        if (sz)
            return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                        "f64 fixed-point VCVT not implemented");
        const uint32_t imm5 = 2 * vm + (m_bit ? 1u : 0u);
        const int fx = 32 - (int)imm5;      // the fraction width, 1..32
        if (fx < 1 || fx > 32)
            return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                        "VCVT fixed-point fraction out of range");
        const double scale = std::ldexp(1.0, fx);
        if (vn == 0xA) {        // Sd = (float)(int32_t)Sd / 2^fx
            set_vfp_s(sd_s, f32_bits((float)((double)(int32_t)vfp_s(sd_s) / scale)));
        } else if (vn == 0xB) { // Sd = (float)(uint32_t)Sd / 2^fx
            set_vfp_s(sd_s, f32_bits((float)((double)vfp_s(sd_s) / scale)));
        } else {                // 0xE signed / 0xF unsigned: Sd = int(Sd * 2^fx)
            const double x = (double)bits_f32(vfp_s(sd_s)) * scale;
            const double r = bit(insn, 7) ? std::nearbyint(x)
                                          : (x < 0 ? std::ceil(x) : std::floor(x));
            bool ioc = false;
            uint32_t out = 0;
            if (std::isnan(x)) { ioc = true; out = 0; }
            else if (vn == 0xE) {
                if (r >= 2147483648.0 || r < -2147483648.0) {
                    ioc = true; out = (r < 0) ? 0x80000000u : 0x7FFFFFFFu;
                } else out = (uint32_t)(int32_t)r;
            } else {
                if (r >= 4294967296.0 || r <= -1.0) {
                    ioc = true; out = (r <= -1.0) ? 0u : 0xFFFFFFFFu;
                } else out = (uint32_t)r;
            }
            if (ioc) vfpscr_ |= 0x00000001u;  // IOC
            set_vfp_s(sd_s, out);
        }
        return Halt::kNone;
    }

    if (vn == 0x0 && q == 0x3) {  // VABS: the sign bit off, NaNs intact
        if (!sz) set_vfp_s(sd_s, vfp_s(sm_s) & 0x7FFFFFFFu);
        else     set_vfp_d(sd_d, vfp_d(sm_d) & 0x7FFFFFFFFFFFFFFFull);
        return Halt::kNone;
    }
    if (vn == 0x0 && q == 0x1) {  // VMOV (register)
        if (!sz) set_vfp_s(sd_s, vfp_s(sm_s));
        else     set_vfp_d(sd_d, vfp_d(sm_d));
        return Halt::kNone;
    }
    if (vn == 0x8) {  // VCVT to floating-point from an integer in Sm
        const uint32_t src = vfp_s(sm_s);
        if (!sz) set_vfp_s(sd_s, f32_bits(bit(insn, 7) ? (float)(int32_t)src
                                                       : (float)src));
        else     set_vfp_d(sd_d, f64_bits(bit(insn, 7) ? (double)(int32_t)src
                                                       : (double)src));
        return Halt::kNone;
    }
    if (vn == 0x4 || vn == 0x5) {  // VCMP with a register / with 0.0
        if (!sz) vfp_compare_f32(vfp_s(sd_s), (vn == 0x4) ? vfp_s(sm_s) : 0u);
        else     vfp_compare_f64(vfp_d(sd_d), (vn == 0x4) ? vfp_d(sm_d) : 0ull);
        return Halt::kNone;
    }
    if (vn == 0x1 && q == 0x3) {  // VSQRT
        if (!sz) {
            const float a = bits_f32(vfp_s(sm_s));
            if (a < 0.0f) vfpscr_ |= 0x00000001u;  // IOC on the negative
            set_vfp_s(sd_s, f32_bits(std::sqrt(a)));
        } else {
            const double a = bits_f64(vfp_d(sm_d));
            if (a < 0.0) vfpscr_ |= 0x00000001u;
            set_vfp_d(sd_d, f64_bits(std::sqrt(a)));
        }
        return Halt::kNone;
    }
    if (vn == 0x1 && q == 0x1) {  // VNEG: the sign bit on, NaNs intact
        if (!sz) set_vfp_s(sd_s, vfp_s(sm_s) ^ 0x80000000u);
        else     set_vfp_d(sd_d, vfp_d(sm_d) ^ 0x8000000000000000ull);
        return Halt::kNone;
    }
    if (vn == 0xD || vn == 0xC) {  // VCVT to an integer in Sd (VCVT / VCVTR)
        const bool to_signed = (vn == 0xD);
        const double x = sz ? bits_f64(vfp_d(sm_d)) : (double)bits_f32(vfp_s(sm_s));
        const double r = bit(insn, 7) ? std::nearbyint(x)
                                      : (x < 0 ? std::ceil(x) : std::floor(x));
        bool ioc = false;
        uint32_t out = 0;
        if (std::isnan(x)) { ioc = true; out = 0; }
        else if (to_signed) {
            if (r >= 2147483648.0 || r < -2147483648.0) {
                ioc = true; out = (r < 0) ? 0x80000000u : 0x7FFFFFFFu;
            } else out = (uint32_t)(int32_t)r;
        } else {
            if (r >= 4294967296.0 || r <= -1.0) {
                ioc = true; out = (r <= -1.0) ? 0u : 0xFFFFFFFFu;
            } else out = (uint32_t)r;
        }
        if (ioc) vfpscr_ |= 0x00000001u;  // IOC
        set_vfp_s(sd_s, out);
        return Halt::kNone;
    }
    if (vn == 0x7) {  // VCVT between f32 and f64 (sz names the SOURCE size)
        if (sz) set_vfp_s(sd_s, f32_bits((float)bits_f64(vfp_d(sm_d))));
        else    set_vfp_d(sd_d, f64_bits((double)bits_f32(vfp_s(sm_s))));
        return Halt::kNone;
    }
    return halt(Halt::kUnknownInstruction, pc_addr, insn,
                "VFP unary instruction not implemented");
}

Arm7Cpu::Halt Arm7Cpu::execute_vfp_transfer(uint32_t pc_addr, uint32_t insn) {
    // The MRC/MCR space: bits 27-24 == 1110, bit 4 == 1, cp 10 or 11.
    const uint32_t cp = bits(insn, 11, 8);
    const uint32_t opc1 = bits(insn, 23, 21);
    const bool l = bit(insn, 20);                // 0: to the VFP, 1: to the core
    const uint32_t crn = bits(insn, 19, 16);
    const uint32_t rt = bits(insn, 15, 12);
    const uint32_t opc2 = bits(insn, 6, 5);      // bit 7 is a register index
    const uint32_t n_bit = bit(insn, 7);
    const uint32_t core = rt == 15 ? pc_addr + 8 : r_[rt];  // pipeline read

    if (opc1 == 0x7) {  // VMSR / VMRS, opc2 == 0
        if (opc2 != 0)
            return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                        "VMSR/VMRS form not implemented");
        if (!l) {  // VMSR: write one of the writable registers
            switch (crn) {
            case 1: vfpscr_ = core; break;              // FPSCR
            case 8: fpexc_ = core; break;               // FPEXC (EN stored,
                                                        // not enforced)
            case 9: fpinst_ = core; break;
            case 10: fpinst2_ = core; break;
            case 0: case 6: case 7: break;              // FPSID/MVFR: read-only
            default:
                return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                            "VMSR to an unknown VFP register");
            }
            return Halt::kNone;
        }
        // VMRS
        uint32_t v = 0;
        switch (crn) {
        case 0: v = 0x410330C0u; break;   // FPSID: the Cortex-A8, VFPv3
        case 1: v = vfpscr_; break;
        case 6: v = 0x00011111u; break;   // MVFR1
        case 7: v = 0x11110222u; break;   // MVFR0
        case 8: v = fpexc_; break;
        case 9: v = fpinst_; break;
        case 10: v = fpinst2_; break;
        default:
            return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                        "VMRS from an unknown VFP register");
        }
        if (rt == 15) {  // VMRS APSR_nzcv, FPSCR: only FPSCR may go to flags
            if (crn != 1)
                return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                            "VMRS to APSR_nzcv needs FPSCR");
            cpsr_ = (cpsr_ & ~0xF0000000u) | (v & 0xF0000000u);
        } else {
            r_[rt] = v;
        }
        return Halt::kNone;
    }

    if (cp == 10 && opc1 == 0x0 && opc2 == 0x0) {  // VMOV Sn, Rt / Rt, Sn
        const uint32_t sn = 2 * crn + (n_bit ? 1u : 0u);
        if (!l) set_vfp_s(sn, core);
        else if (rt != 15) r_[rt] = vfp_s(sn);   // to r15: unpredictable
        return Halt::kNone;
    }

    if (cp == 11 && opc2 == 0x0 && opc1 <= 0x1) {  // VMOV.32 Dd[x], Rt / back
        const uint32_t dd = (bit(insn, 22) ? 16u : 0u) + crn;
        const int lane = (int)opc1;
        if (!l) {
            const uint64_t shift = 32ull * (unsigned)lane;
            set_vfp_d(dd, (vfp_d(dd) & ~(0xFFFFFFFFull << shift)) |
                          ((uint64_t)core << shift));
        } else if (rt != 15) {
            r_[rt] = (uint32_t)(vfp_d(dd) >> (32 * lane));
        }
        return Halt::kNone;
    }

    return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                "VFP register transfer not implemented");
}

Arm7Cpu::Halt Arm7Cpu::execute_vfp_ldst(uint32_t pc_addr, uint32_t insn) {
    // The LDC/STC space: bits 27-25 == 110, bit 4 == 0, cp 10 or 11. P is
    // bit 24, U 23, the register-size bit 22, W 21, L 20.
    const bool p = bit(insn, 24), u = bit(insn, 23);
    const bool d_bit = bit(insn, 22), w = bit(insn, 21), l = bit(insn, 20);
    const uint32_t rn = bits(insn, 19, 16);
    const uint32_t vd = bits(insn, 15, 12);
    const bool sz = bit(insn, 8);              // 0: single, 1: double
    const uint32_t imm8 = bits(insn, 7, 0);
    // The first register: s-regs put the extra bit low, d-regs high.
    const uint32_t first = sz ? ((d_bit ? 16u : 0u) + vd)
                              : (2 * vd + (d_bit ? 1u : 0u));

    if (p && !w) {  // VLDR / VSTR: one register, imm8 * 4 off the base
        const uint32_t base = rn == 15 ? pc_addr + 8 : r_[rn];
        const uint32_t addr = u ? base + 4u * imm8 : base - 4u * imm8;
        if (l) {
            uint32_t lo = 0, hi = 0;
            if (!rd32(addr, &lo)) return data_fault(pc_addr, insn);
            if (sz && !rd32(addr + 4, &hi)) return data_fault(pc_addr, insn);
            if (!sz) set_vfp_s(first, lo);
            else set_vfp_d(first, (uint64_t)lo | ((uint64_t)hi << 32));
        } else {
            if (!sz) {
                if (!wr32(addr, vfp_s(first))) return data_fault(pc_addr, insn);
            } else {
                if (!wr32(addr, (uint32_t)vfp_d(first))) return data_fault(pc_addr, insn);
                if (!wr32(addr + 4, (uint32_t)(vfp_d(first) >> 32)))
                    return data_fault(pc_addr, insn);
            }
        }
        return Halt::kNone;
    }

    // VLDM / VSTM: imm8 counts WORDS (an s-register is one, a d-register
    // two). The addressing follows LDM/STM's P/U/W exactly.
    if (rn == 15)
        return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                    "VLDM/VSTM with the PC as base");
    if (sz && (imm8 & 1u))
        return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                    "VLDM/VSTM of d registers with an odd word count");
    const uint32_t words = imm8;
    const uint32_t nregs = sz ? words / 2 : words;
    const uint32_t base = r_[rn];
    uint32_t addr = u ? (p ? base + 4 : base)
                      : (p ? base - 4 * words : base - 4 * (words - (words ? 1 : 0)));
    // (the empty list leaves the base alone below; words == 0 is a no-op)

    if (w && words) r_[rn] = u ? base + 4 * words : base - 4 * words;

    for (uint32_t k = 0; k < nregs; ++k) {
        if (!sz) {
            if (l) {
                uint32_t v = 0;
                if (!rd32(addr, &v)) return data_fault(pc_addr, insn);
                set_vfp_s(first + k, v);
            } else {
                if (!wr32(addr, vfp_s(first + k))) return data_fault(pc_addr, insn);
            }
            addr += 4;
        } else {
            const uint32_t dd = first + k;
            if (l) {
                uint32_t lo = 0, hi = 0;
                if (!rd32(addr, &lo)) return data_fault(pc_addr, insn);
                if (!rd32(addr + 4, &hi)) return data_fault(pc_addr, insn);
                set_vfp_d(dd, (uint64_t)lo | ((uint64_t)hi << 32));
            } else {
                if (!wr32(addr, (uint32_t)vfp_d(dd))) return data_fault(pc_addr, insn);
                if (!wr32(addr + 4, (uint32_t)(vfp_d(dd) >> 32)))
                    return data_fault(pc_addr, insn);
            }
            addr += 8;
        }
    }
    return Halt::kNone;
}

Arm7Cpu::Halt Arm7Cpu::execute_vfp_move2(uint32_t pc_addr, uint32_t insn) {
    // The MCRR/MRRC space: bits 27-24 == 1100, bit 4 == 1, cp 10, opc1 010.
    // VMOV Dd, Rt, Rt2 / VMOV Rt, Rt2, Dd -- the only guest the kernel's
    // VFP state save and restore uses for whole registers.
    const bool l = bit(insn, 20);
    const uint32_t rt2 = bits(insn, 19, 16), rt = bits(insn, 15, 12);
    const uint32_t dd = (bit(insn, 5) ? 16u : 0u) + bits(insn, 3, 0);
    if (!l) {  // to the VFP: Rt the low word, Rt2 the high word
        const uint32_t lo = rt == 15 ? pc_addr + 8 : r_[rt];
        const uint32_t hi = rt2 == 15 ? pc_addr + 8 : r_[rt2];
        set_vfp_d(dd, (uint64_t)lo | ((uint64_t)hi << 32));
    } else {
        if (rt != 15) r_[rt] = (uint32_t)vfp_d(dd);
        if (rt2 != 15) r_[rt2] = (uint32_t)(vfp_d(dd) >> 32);
    }
    return Halt::kNone;
}

// -- CPS / SETEND ------------------------------------------------------------------

Arm7Cpu::Halt Arm7Cpu::execute_cps(uint32_t pc_addr, uint32_t insn) {
    // SETEND (bit 16 set): this machine has one endianness; a no-op.
    if (bit(insn, 16)) return Halt::kNone;
    const uint32_t imod = bits(insn, 19, 18);   // 2: cpsie, 3: cpsid
    const bool has_mode = bit(insn, 17);
    if (has_mode) {
        const uint32_t to = insn & 0x1Fu;
        if (mode_index(to) < 0)
            return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                        "invalid mode in CPS");
        if (to != mode()) switch_mode(mode(), to);
        cpsr_ = (cpsr_ & ~0x1Fu) | to;
    }
    // The A/I/F masks (bits 8:6) are stored and reported; this machine has
    // no interrupt source, so there is nothing they can hold off. cpsid
    // SETS the named masks (disables), cpsie clears them.
    if (imod == 0x2 || imod == 0x3) {
        const uint32_t aif = bits(insn, 8, 6) << 6;
        if (imod == 0x3) cpsr_ |= aif;    // cpsid
        else             cpsr_ &= ~aif;   // cpsie
    }
    return Halt::kNone;
}

// -- dispatch ------------------------------------------------------------------

Arm7Cpu::Halt Arm7Cpu::execute(uint32_t pc_addr, uint32_t insn) {
    const uint32_t cond = insn >> 28;
    if (cond != 0xE && !cond_pass(cond)) return Halt::kNone;

    // The unconditional space (cond 0xF), before the group decode: the
    // barriers and prefetch hints the kernel's own code uses, and the
    // VFP-shaped encodings this CPU does not have. In a strictly serial
    // interpreter a barrier is honoured by construction; prefetch is a
    // promise the machine needs no help keeping.
    if (cond == 0xF) {
        const uint32_t g = (insn >> 25) & 7u;
        if ((insn & 0x0FF0F000u) == 0x0570F000u)
            return Halt::kNone;                    // DSB/DMB/ISB (0xF57FF0xx)
        if (g == 0x2 || g == 0x3) {                // LDR/STR-shaped:
            if (bits(insn, 15, 12) == 0xF)
                return Halt::kNone;                // PLD/PLI/PLDW, Rt = 0xF
            return halt(Halt::kUnknownInstruction, pc_addr, insn,
                        "unconditional load/store (VFP) not implemented");
        }
        if (g == 0x7 && !bit(insn, 24) && !bit(insn, 4))
            return halt(Halt::kUnknownInstruction, pc_addr, insn,
                        "unconditional coprocessor instruction not implemented");
        // CPS and SETEND ride in the unconditional space at bits 27-24 ==
        // 0001 -- and so do both interrupt forms of CPS (bits 27-20 ==
        // 0001 11xx), which an earlier, narrower mask missed, running
        // `cpsid i` as a BIC and clobbering a register. The whole space goes
        // to execute_cps, which honours a mode change with the banked file.
        if ((insn & 0x0F000000u) == 0x01000000u)   // CPS / SETEND
            return execute_cps(pc_addr, insn);
        // BLX immediate (g 5) and anything else falls through to the
        // conditional groups, which report it in their own words
    }

    const uint32_t grp = (insn >> 25) & 7u;

    if (grp == 0x1) {  // data processing (immediate) -- or MSR immediate
        // The hint space rides in the MSR slot: hints (0xE320F00x) carry
        // mask = 0b0000 (no fields), Rd = 0b1111 and bits 11-4 zero; a real
        // 'msr cpsr_<fields>, #imm' always names a field, so its mask nibble
        // is nonzero and it never matches.
        if ((insn & 0x0FFFFFF0u) == 0x0320F000u) {  // the hint space (mask 0)
            if ((insn & 0xFu) == 3)  // WFI: this machine has no interrupt
                return halt(Halt::kWaitForInterrupt, pc_addr, insn,
                            "wfi: no interrupt source exists in this machine");
            return Halt::kNone;      // NOP, YIELD, WFE, SEV
        }
        if ((insn & 0x0FF0F000u) == 0x0320F000u) {  // MSR cpsr, #imm
            bool carry = false;
            const uint32_t v = imm_operand2(insn, &carry);
            return execute_msr(insn, v);
        }
        // MOVW/MOVT (ARMv7): the way every address constant is built on
        // armv7 -- XNU's start uses them for its literal-pointer arithmetic.
        // Bits 27-20 are 0x30/0x34 with the S bit CLEAR; the S bit set is
        // what separates them from TST (0x31) and CMP (0x35), so the mask
        // must cover bit 20 -- a mask without it ran every 'cmp rN, #imm'
        // as MOVT and clobbered the register (the conds rig caught it).
        if ((insn & 0x0FF00000u) == 0x03000000u) {   // MOVW: Rd = imm16
            r_[bits(insn, 15, 12)] = ((insn >> 4) & 0xF000u) | (insn & 0x0FFFu);
            return Halt::kNone;
        }
        if ((insn & 0x0FF00000u) == 0x03400000u) {   // MOVT: Rd |= imm16 << 16
            const uint32_t rd = bits(insn, 15, 12);
            r_[rd] = (r_[rd] & 0x0000FFFFu) |
                     ((((insn >> 4) & 0xF000u) | (insn & 0x0FFFu)) << 16);
            return Halt::kNone;
        }
        if ((insn & 0x0FF0F000u) == 0x0360F000u) {  // MSR spsr, #imm
            bool carry = false;
            const uint32_t v = imm_operand2(insn, &carry);
            return execute_msr(insn, v, true);
        }
        return execute_dp(pc_addr, insn);
    }

    if (grp == 0x0) {
        // -- exclusion-space patterns (checked before the generic decode) --
        if ((insn & 0x0FFFFFF0u) == 0x012FFF10u) {  // BX
            const uint32_t v = r_[insn & 0xFu];
            if (v & 1u) return halt(Halt::kThumbUnsupported, pc_addr, insn, "BX to a Thumb address");
            r_[15] = v & ~1u;
            return Halt::kNone;
        }
        if ((insn & 0x0FFFFFF0u) == 0x012FFF30u)  // BLX (register)
            return halt(Halt::kThumbUnsupported, pc_addr, insn, "BLX register: Thumb switch");

        if ((insn & 0x0FFF0FFFu) == 0x010F0000u) {  // MRS cpsr
            r_[bits(insn, 15, 12)] = cpsr_;
            return Halt::kNone;
        }
        if ((insn & 0x0FFF0FFFu) == 0x014F0000u) {  // MRS spsr
            const int mi = mode_index(mode());
            if (mi <= 0)
                return halt(Halt::kUnsupportedEncoding, pc_addr, insn,
                            "MRS spsr in a mode without one");
            r_[bits(insn, 15, 12)] = bank_spsr_[mi];
            return Halt::kNone;
        }

        if ((insn & 0x0FF0FFF0u) == 0x0120F000u)  // MSR cpsr, <Rm>
            return execute_msr(insn, r_[insn & 0xFu]);
        if ((insn & 0x0FF0FFF0u) == 0x0160F000u)  // MSR spsr, <Rm>
            return execute_msr(insn, r_[insn & 0xFu], true);

        if ((insn & 0x0FFF0FF0u) == 0x016F0F10u) {  // CLZ
            r_[bits(insn, 15, 12)] = (uint32_t)clz32(r_[insn & 0xFu]);
            return Halt::kNone;
        }

        const bool b7 = bit(insn, 7), b4 = bit(insn, 4);
        if (b7 && b4) {
            const uint32_t opn = bits(insn, 23, 20);
            const uint32_t nib74 = bits(insn, 7, 4);
            // The multiply space is bits 27-22 = 000000 with bits 7-4 = 1001;
            // without that 0x9 the test below swallows every *post-indexed*
            // extra load/store (bit 24 = 0) whose opcode nibble happens to
            // match, and `strh r2, [r9], #2` executes as an SMULL -- the
            // stage-8 demo kernel watched its framebuffer pointer turn into
            // a multiply result that way.
            if (nib74 == 0x9 && !bit(insn, 24) &&
                (opn == 0x0 || opn == 0x2 || opn == 0x8 || opn == 0xA || opn == 0xC || opn == 0xE))
                return execute_multiply(insn);
            if (nib74 == 0xB || nib74 == 0xD || nib74 == 0xF)
                return execute_ldst_ext(pc_addr, insn);
            return halt(Halt::kUnknownInstruction, pc_addr, insn, "extension-space instruction not implemented");
        }
        // A set bit 7 with bit 4 clear is nothing of its own: it is the low
        // bit of a 5-bit immediate shift amount (lsl #5 assembles to operand2
        // 0x282), and only bit 4 selects the register-shifted form, which
        // dp_operand2() decodes. Rejecting bit 7 here -- as this decoder once
        // did, until the stage-8 demo kernel assembled `lsl r2, r2, #5` and
        // halted on it -- turns every odd shift amount into an error.

        if (b4) {
            // register-shifted data processing -- unless the opcode byte is
            // one of the misc-space values (TST/TEQ/CMP/CMN with S=0, and
            // friends), where only the handled patterns above are legal.
            const uint32_t byte27_20 = (insn >> 20) & 0xFFu;
            if (byte27_20 == 0x10 || byte27_20 == 0x12 || byte27_20 == 0x14 || byte27_20 == 0x16)
                return halt(Halt::kUnknownInstruction, pc_addr, insn, "misc-space instruction not implemented");
        }
        return execute_dp(pc_addr, insn);
    }

    if (grp == 0x2) return execute_ldst(pc_addr, insn, true);

    if (grp == 0x3) {
        if (!bit(insn, 7) && !bit(insn, 4)) return execute_ldst(pc_addr, insn, false);
        return halt(Halt::kUnknownInstruction, pc_addr, insn, "media instruction not implemented");
    }

    if (grp == 0x4) return execute_ldm_stm(pc_addr, insn);

    if (grp == 0x5) {  // B / BL, and BLX (immediate) when cond == 0xF
        if (cond == 0xF) return halt(Halt::kThumbUnsupported, pc_addr, insn, "BLX immediate: Thumb switch");
        if (bit(insn, 24)) r_[14] = pc_addr + 4;  // BL
        const int32_t imm24 = (int32_t)(insn << 8) >> 8;
        r_[15] = pc_addr + 8 + ((uint32_t)(imm24 << 2));
        return Halt::kNone;
    }

    if (grp == 0x6) {  // the coprocessor memory space, bits 27-25 == 110
        const uint32_t cp = bits(insn, 11, 8);
        if (bit(insn, 4)) {  // MCRR/MRRC: a register-pair transfer
            if ((cp == 10 || cp == 11) && bits(insn, 23, 21) == 0x2)
                return execute_vfp_move2(pc_addr, insn);   // VMOV Dd, Rt, Rt2
            return halt(Halt::kUnknownInstruction, pc_addr, insn,
                        "coprocessor register-pair instruction not implemented");
        }
        if (cp == 10 || cp == 11)                            // VLDR/VSTR/VLDM/VSTM
            return execute_vfp_ldst(pc_addr, insn);
        return halt(Halt::kUnknownInstruction, pc_addr, insn,
                    "coprocessor load/store (LDC/STC) not implemented");
    }

    // grp == 7: the coprocessor register transfers live here (MRC/MCR are
    // bits 27-24 = 1110 with bit 4 set), then SVC (bit 24 = 1, a real
    // condition in the field), then the coprocessor data operations (CDP,
    // bits 27-24 = 1110 with bit 4 clear).
    if (bit(insn, 4) && bits(insn, 27, 24) == 0xE) {  // MRC / MCR
        const uint32_t cp = bits(insn, 11, 8);
        if (cp == 15)
            return execute_cp15(pc_addr, insn);       // the A4's system coprocessor
        if (cp == 10 || cp == 11)
            return execute_vfp_transfer(pc_addr, insn);  // VMSR/VMRS, VMOV
        return halt(Halt::kUnknownInstruction, pc_addr, insn,
                    "coprocessor register transfer not implemented");
    }
    if (bit(insn, 24))  // SVC
        return halt(Halt::kSupervisorCall, pc_addr, insn, "supervisor call (trap handling is stage-6 work)");
    if (bits(insn, 27, 24) == 0xE) {  // CDP: the VFP's own arithmetic
        const uint32_t cp = bits(insn, 11, 8);
        if (cp == 10 || cp == 11)
            return execute_vfp_dp(pc_addr, insn);
        return halt(Halt::kUnknownInstruction, pc_addr, insn,
                    "coprocessor data operation not implemented");
    }
    return halt(Halt::kUnknownInstruction, pc_addr, insn, "coprocessor instruction not implemented");
}

// -- top level -----------------------------------------------------------------

Arm7Cpu::Halt Arm7Cpu::step() {
    if (info_.halt != Halt::kNone) return info_.halt;

    const uint32_t pc_addr = r_[15];
    if (pc_addr & 3u) {
        info_.halt = Halt::kMisalignedPc;
        info_.halt_pc = pc_addr;
        return Halt::kMisalignedPc;
    }
    if (mem_.base() == nullptr || (uint64_t)pc_addr + 4 > mem_.size()) {
        info_.halt = Halt::kFetchFault;
        info_.halt_pc = pc_addr;
        return Halt::kFetchFault;
    }

    const uint32_t insn = load_u32(mem_.base() + pc_addr);
    r_[15] = pc_addr + 4;
    ++info_.executed;

    return execute(pc_addr, insn);
}

Arm7Cpu::Halt Arm7Cpu::run(uint64_t max_steps) {
    for (uint64_t i = 0; i < max_steps; ++i) {
        const Halt h = step();
        if (h != Halt::kNone) return h;
    }
    return Halt::kNone;
}

}  // namespace hakios
