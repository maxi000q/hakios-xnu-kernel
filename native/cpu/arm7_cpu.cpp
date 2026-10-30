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
    }
    return "?";
}

void Arm7Cpu::reset(uint32_t pc) {
    std::memset(r_, 0, sizeof r_);
    r_[15] = pc;
    cpsr_ = 0x13;  // SVC mode, flags clear, ARM state
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
                // MOVS pc / LDM-with-S: the SPSR restore, needs banked state.
                return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "MOVS pc, <x>: SPSR restore not implemented");
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
    if (nib == 0xD && !l) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "LDRD");
    if (nib == 0xF && !l) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "STRD");

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

Arm7Cpu::Halt Arm7Cpu::execute_ldm_stm(uint32_t pc_addr, uint32_t insn) {
    const bool p = bit(insn, 24), u = bit(insn, 23), s = bit(insn, 22),
               w = bit(insn, 21), l = bit(insn, 20);
    const uint16_t list = (uint16_t)(insn & 0xFFFFu);
    const uint32_t rn = bits(insn, 19, 16);

    if (s) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "LDM^/STM^ user-bank transfer");
    if (list == 0) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "LDM/STM with empty register list");
    if (rn == 15) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "LDM/STM with PC base");
    if ((list >> rn) & 1u) return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "LDM/STM with Rn in the list");

    const int count = popcount16(list);
    const uint32_t base_v = r_[rn];
    uint32_t addr = u ? (p ? base_v + 4 : base_v)
                      : (p ? base_v - 4 * count : base_v - 4 * (count - 1));

    if (w) r_[rn] = u ? base_v + 4 * count : base_v - 4 * count;

    for (int i = 0; i < 16; ++i) {
        if (!((list >> i) & 1u)) continue;
        if (l) {
            uint32_t v = 0;
            if (!rd32(addr, &v)) return data_fault(pc_addr, insn);
            if (i == 15) r_[15] = v & ~3u;  // LDM {pc}: branch (documented mask)
            else r_[i] = v;
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

Arm7Cpu::Halt Arm7Cpu::execute_msr(uint32_t insn, uint32_t value) {
    const uint32_t mask_nib = bits(insn, 19, 16);
    uint32_t mask = 0;
    if (mask_nib & 1u) mask |= 0x000000FFu;  // c: control
    if (mask_nib & 2u) mask |= 0x0000FF00u;  // x: extension
    if (mask_nib & 4u) mask |= 0x00FF0000u;  // s: status
    if (mask_nib & 8u) mask |= 0xFF000000u;  // f: flags
    cpsr_ = (cpsr_ & ~mask) | (value & mask);
    if (cpsr_ & 0x20u)  // T bit: Thumb state switch
        return halt(Halt::kThumbUnsupported, 0, insn, "MSR set the Thumb bit");
    return Halt::kNone;
}

// -- dispatch ------------------------------------------------------------------

Arm7Cpu::Halt Arm7Cpu::execute(uint32_t pc_addr, uint32_t insn) {
    const uint32_t cond = insn >> 28;
    if (cond != 0xE && !cond_pass(cond)) return Halt::kNone;

    const uint32_t grp = (insn >> 25) & 7u;

    if (grp == 0x1) {  // data processing (immediate) -- or MSR immediate
        if ((insn & 0x0FF0F000u) == 0x0320F000u) {  // MSR cpsr, #imm
            bool carry = false;
            const uint32_t v = imm_operand2(insn, &carry);
            return execute_msr(insn, v);
        }
        if ((insn & 0x0FF0F000u) == 0x0360F000u)  // MSR spsr, #imm
            return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "MSR spsr: no banked state yet");
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
        if ((insn & 0x0FFF0FFFu) == 0x014F0000u)  // MRS spsr
            return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "MRS spsr: no banked state yet");

        if ((insn & 0x0FF0FFF0u) == 0x0120F000u)  // MSR cpsr, <Rm>
            return execute_msr(insn, r_[insn & 0xFu]);
        if ((insn & 0x0FF0FFF0u) == 0x0160F000u)  // MSR spsr, <Rm>
            return halt(Halt::kUnsupportedEncoding, pc_addr, insn, "MSR spsr: no banked state yet");

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

    if (grp == 0x6) return halt(Halt::kUnknownInstruction, pc_addr, insn, "coprocessor instruction not implemented");

    // grp == 7: bits 27-24 == 1111 is SVC (its condition field is a real
    // condition, usually AL -- the 1111 lives in bits 27-24, not in cond).
    if (bit(insn, 24))  // SVC
        return halt(Halt::kSupervisorCall, pc_addr, insn, "supervisor call (trap handling is stage-6 work)");
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
