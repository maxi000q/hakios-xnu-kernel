#!/usr/bin/env python3
"""Generate tests/gen/arm_progs.h: ARMv7 test programs, assembled by keystone.

Every instruction the stage-3 interpreter is verified against comes out of a
battle-tested assembler, so a decode bug cannot hide behind a hand-encoded
test word. Programs end with UDF (0xE7F000F0), which the interpreter halts on
as kUnknownInstruction -- the tests treat that as "ran to the end".

A few words keystone cannot express (RRX) are packed as raw words.

Run:  python3 scripts/gen_arm_tests.py     (writes tests/gen/arm_progs.h)
      python3 scripts/gen_arm_tests.py --check   (fail if the file is stale)
"""

import struct
import sys
import os

from keystone import Ks, KS_ARCH_ARM, KS_MODE_ARM

UDF = 0xE7F000F0  # permanently undefined -- our clean "end of program"

# (name, org, body, raw_words) -- raw_words are appended verbatim after the
# assembled body, before the UDF sentinel.
PROGRAMS = [
    (
        # flags are snapshotted into registers with mrs right after each
        # flag-setting instruction -- the checks then read the snapshots,
        # not the final state
        "flags_data_proc",
        0x1000,
        """
        mov r0, #0xFF
        orr r0, r0, #0x100
        movs r1, r0, lsl #24
        mrs r2, cpsr
        cmp r1, r1
        mrs r3, cpsr
        mvn r5, #0x80000000
        adds r5, r5, #1
        mrs r6, cpsr
        subs r7, r7, #0
        mrs r8, cpsr
        mvn r9, #0
        """,
        [],
    ),
    (
        "loop_bne",
        0x1000,
        """
        mov r1, #5
        loop:
            subs r1, r1, #1
            bne loop
        mov r2, r1
        """,
        [],
    ),
    (
        # PC-as-base loads: reads of r15 give insn+8, so [pc,#-4] at 0x1004
        # reads the word at 0x1008 (the mov r3 below, asserted from the
        # generated table), and [pc,#0] at 0x100C reads 0x1014: the UDF.
        "literal_pool",
        0x1000,
        """
        mov r0, #0
        ldr r1, [pc, #-4]
        mov r3, #7
        ldr r2, [pc, #0]
        mov r4, r0
        """,
        [],
    ),
    (
        "mem_bytes",
        0x1000,
        """
        mov r0, #0x200000
        mov r1, #0x100
        str r1, [r0]
        ldr r2, [r0]
        mov r3, #0xAB
        strb r3, [r0, #4]
        ldrb r4, [r0, #4]
        mov r5, #0x1200
        add r5, r5, #0x34
        mov r14, #0x18
        strh r5, [r0, r14]
        ldrh r12, [r0, r14]
        ldrsh r13, [r0, r14]
        strh r5, [r0, #0x10]
        ldrh r6, [r0, #0x10]
        mov r7, #0x80
        strb r7, [r0, #0x14]
        ldrsb r8, [r0, #0x14]
        ldrh r9, [r0, #0x14]
        ldrsh r10, [r0, #0x14]
        add r11, r0, #0x100
        str r1, [r11], #4
        ldr r12, [r11, #-4]!
        """,
        [],
    ),
    (
        "ldm_stm",
        0x1000,
        """
        mov r0, #0x11
        mov r1, #0x22
        mov r2, #0x33
        mov r10, #0x200000
        add r10, r10, #0x100
        stmia r10, {r0-r2}
        add r11, r10, #12
        ldmdb r11!, {r3-r5}
        stmdb r11!, {r0-r2}
        ldmia r11, {r6-r8}
        mov r9, #0x200000
        add r9, r9, #0x200
        stmia r9, {r0-r2}
        add r12, r9, #8
        ldmda r12!, {r13-r14}
        """,
        [],
    ),
    (
        "mul_clz",
        0x1000,
        """
        mov r0, #100
        mov r1, #7
        mul r2, r0, r1
        mov r5, #1000
        mla r3, r0, r1, r5
        umull r4, r6, r0, r1
        mvn r7, #99
        smull r8, r9, r7, r1
        mov r10, #0x100
        clz r11, r10
        mov r12, #0
        clz r13, r12
        """,
        [],
    ),
    (
        "mrs_msr",
        0x1000,
        """
        mrs r0, cpsr
        msr cpsr_c, #0x10
        mrs r1, cpsr
        msr cpsr_f, #0x20000000
        mrs r2, cpsr
        msr cpsr_c, r0
        mrs r3, cpsr
        """,
        [],
    ),
    (
        # MOVW/MOVT build every address constant on armv7 -- and their
        # encodings sit one S bit away from TST (0x31) and CMP (0x35), which
        # is why the cmp instructions ride along: an interpreter that mixes
        # them up clobbers a register on every compare (the rig caught
        # exactly that when these were added).
        "movw_movt",
        0x1000,
        """
        movw r0, #0xc404
        movt r0, #0x24
        movw r1, #0xbeef
        cmp r0, #0
        movne r2, #0x55
        cmp r1, r1
        moveq r3, #0xAA
        movt r1, #0xdead
        """,
        [],
    ),
    (
        # The A4's system coprocessor and the barrier/hint space: identity
        # reads with the model values, a write/read-back pair through TTBR0,
        # and the barriers/hints the kernel's own code is full of.
        "cp15_system",
        0x1000,
        """
        mrc p15, 0, r0, c0, c0, 0
        mrc p15, 0, r1, c0, c0, 5
        mrc p15, 0, r2, c1, c0, 0
        mov r3, #0x123
        mcr p15, 0, r3, c2, c0, 0
        mrc p15, 0, r4, c2, c0, 0
        dsb sy
        dmb sy
        isb sy
        nop
        wfe
        pld [r0]
        mov r5, #7
        """,
        [],
    ),
    (
        # WFI in a machine with no interrupt source is a clean, named halt:
        # the instruction after it must never run.
        "wfi",
        0x1000,
        """
        mov r0, #1
        wfi
        mov r0, #2
        """,
        [],
    ),
    (
        "conds",
        0x1000,
        """
        mov r0, #0
        cmp r0, #0
        beq t1
        mov r1, #0xFF
        t1:
        cmp r0, #1
        blt t2
        mov r2, #0xFF
        t2:
        cmp r0, #0
        bhi t3
        mov r3, #0x55
        t3:
        cmp r0, #0
        bge t4
        mov r4, #0xFF
        t4:
        movs r5, #1
        bvs t5
        mov r6, #0xAA
        t5:
        """,
        [],
    ),
    (
        "vram_through_cpu",
        0x1000,
        """
        mov r0, #0x20000000
        ldr r1, [pc, #4]
        b over
        over:
        str r1, [r0]
        """,
        [0xDEADBEEF],  # literal at 0x100C: (insn+8)+4, right after the b
    ),
    (
        # keystone assembles "rors r6, r3, #0" as a plain LSL #0, so RRX --
        # ROR with imm5 == 0, the C-in rotate -- goes in as a raw word.
        "rrx_raw",
        0x1000,
        """
        msr cpsr_f, #0x20000000
        mov r3, #1
        """,
        [0xE1B06063],  # rors r6, r3, rrx
    ),
    (
        "reg_shift",
        0x1000,
        """
        mov r4, #4
        mov r0, #0x18
        movs r1, r0, lsl r4
        movs r2, r0, lsr r4
        msr cpsr_f, #0
        movs r5, r0, lsl r4
        """,
        [],
    ),
    (
        "halt_svc",
        0x1000,
        """
        svc #0x80
        mov r0, #1
        """,
        [],
    ),
    (
        "halt_undef_coproc",
        0x1000,
        "",
        [0xEE000010],  # MCR p0, ... : coprocessor, not implemented
    ),
    (
        "halt_media",
        0x1000,
        "",
        [0xE6800130],  # parallel-add media space: bits 27-25 == 011
    ),
    (
        "bx_thumb",
        0x1000,
        """
        mov r0, #0x1000
        add r0, r0, #1
        bx r0
        mov r1, #1
        """,
        [],
    ),
    (
        # jumps into the guest RAM area at 0x200000, where the test placed
        # a UDF word beforehand -- proving cross-region branch + fetch
        "bx_arm",
        0x1000,
        """
        mov r0, #0x200000
        bx r0
        mov r1, #1
        """,
        [],
    ),
    (
        # The VFP, f32: immediates, arithmetic, the high bank of s16-s31,
        # and results read back through vmov r, s. sqrt(6) is checked in
        # the test against the host's own float conversion.
        "vfp_arith",
        0x1000,
        """
        vmov.f32 s0, #2.0
        vmov.f32 s1, #3.0
        vadd.f32 s2, s0, s1
        vsub.f32 s3, s1, s0
        vmul.f32 s4, s0, s1
        vdiv.f32 s5, s1, s0
        vsqrt.f32 s6, s4
        vneg.f32 s7, s3
        vabs.f32 s8, s7
        mov r10, #0
        vmov s9, r10
        vmla.f32 s9, s0, s1
        vmov.f32 s16, #2.0
        vadd.f32 s17, s16, s1
        vmov r0, s2
        vmov r1, s3
        vmov r2, s4
        vmov r3, s5
        vmov r4, s6
        vmov r5, s7
        vmov r6, s8
        vmov r7, s9
        vmov r8, s17
        """,
        [],
    ),
    (
        # The VFP, f64: immediates, arithmetic, and the two-core-register
        # vmov that reads a d register out.
        "vfp_d64",
        0x1000,
        """
        vmov.f64 d0, #1.0
        vmov.f64 d1, #2.0
        vadd.f64 d2, d0, d1
        vsub.f64 d3, d1, d0
        vmul.f64 d4, d0, d1
        vdiv.f64 d5, d1, d0
        vmov r0, r1, d2
        vmov r2, r3, d3
        vmov r4, r5, d4
        vmov r6, r7, d5
        """,
        [],
    ),
    (
        # The VFP loads and stores: single and double, immediate offsets in
        # both directions, the multiple forms, and vpush/vpop over a real
        # stack. The memory itself is checked in the test.
        "vfp_ldst",
        0x1000,
        """
        movw r10, #0x0000
        movt r10, #0x20
        movw r13, #0x1000
        movt r13, #0x20
        vmov.f32 s0, #2.0
        vmov.f32 s1, #3.0
        vstr s0, [r10]
        vstmia r10, {s0-s1}
        vldr s2, [r10]
        vldr s3, [r10, #4]
        vldmia r10, {s4-s5}
        vmov r0, s2
        vmov r1, s3
        vmov r2, s4
        vmov r3, s5
        vmov.f64 d0, #1.0
        vstr d0, [r10, #16]
        vldr d1, [r10, #16]
        vmov r4, r5, d1
        vpush {d0}
        vpop {d2}
        vmov r6, r7, d2
        """,
        [],
    ),
    (
        # The VFP system registers: the Cortex-A8 identity the kernel's
        # vfp_init reads, the compare path with its FPSCR -> APSR move, and
        # FPEXC stored and read back.
        "vfp_sysregs",
        0x1000,
        """
        vmrs r0, fpsid
        vmrs r1, mvfr0
        vmrs r2, mvfr1
        vmov.f32 s0, #1.0
        vmov.f32 s1, #1.0
        vcmp.f32 s0, s1
        vmrs APSR_nzcv, fpscr
        mrs r3, cpsr
        vmov.f32 s3, #-1.0
        vcmp.f32 s3, s0
        vmrs APSR_nzcv, fpscr
        mrs r4, cpsr
        vmrs r5, fpscr
        vmsr fpexc, r0
        vmrs r6, fpexc
        """,
        [],
    ),
    (
        # The conversions: integer to float and back with both roundings,
        # f32 to f64 and back, an unsigned 32-bit source, the saturation of
        # an out-of-range conversion, and a fixed-point fraction.
        "vfp_cvt",
        0x1000,
        """
        mov r0, #7
        vmov s0, r0
        vcvt.f32.s32 s1, s0
        vmov r1, s1
        vcvt.s32.f32 s2, s1
        vmov r2, s2
        vmov.f32 s3, #1.5
        vcvt.s32.f32 s4, s3
        vmov r3, s4
        vcvtr.s32.f32 s5, s3
        vmov r4, s5
        vcvt.f64.f32 d0, s1
        vcvt.f32.f64 s6, d0
        vmov r5, s6
        mvn r6, #0
        vmov s7, r6
        vcvt.f32.u32 s8, s7
        vmov r7, s8
        vcvt.u32.f32 s9, s8
        vmov r8, s9
        vmov.f32 s11, #6.0
        vcvt.s32.f32 s11, s11, #2
        vmov r9, s11
        """,
        [],
    ),
    (
        # LDRD/STRD: an even register pair, eight bytes, and the memory
        # checked word by word in the test.
        "ldrd_strd",
        0x1000,
        """
        movw r0, #0x0000
        movt r0, #0x20
        movw r2, #0x3412
        movt r2, #0x7856
        movw r3, #0x0F0E
        movt r3, #0x9876
        strd r2, r3, [r0]
        ldrd r4, r5, [r0]
        strd r2, r3, [r0, #8]
        ldrd r6, r7, [r0, #8]
        """,
        [],
    ),
    (
        # The banked register file: r13 per mode, the FIQ bank of r8-r12,
        # an SPSR written and read, and the exception return (movs pc with
        # the SPSR restoring the Z flag and the mode).
        "banked_modes",
        0x1000,
        """
        mov r13, #0x1001
        msr cpsr_c, #0x12
        mov r13, #0x2002
        msr cpsr_c, #0x13
        mov r1, r13
        msr cpsr_c, #0x12
        mov r2, r13
        msr cpsr_c, #0x13
        msr cpsr_c, #0x11
        mov r8, #0x77
        mov r13, #0x3301
        msr cpsr_c, #0x13
        mov r8, #0x88
        mov r3, r8
        msr cpsr_c, #0x11
        mov r4, r8
        mov r5, r13
        msr cpsr_c, #0x13
        movw r0, #0x0013
        movt r0, #0x6000
        msr spsr_fsxc, r0
        mrs r6, spsr
        mov r7, pc
        add r7, r7, #4
        movs pc, r7
        mrs r8, cpsr
        """,
        [],
    ),
    (
        # The user-bank transfer: stmia {r13}^ stores the USER bank's r13
        # while in SVC, ldmia {r13}^ loads into the USER bank leaving the
        # SVC view untouched -- checked by switching to USR and reading it.
        "ldm_user_bank",
        0x1000,
        """
        movw r2, #0x0000
        movt r2, #0x20
        movw r0, #0x0001
        movt r0, #0xAAAA
        msr cpsr_c, #0x10
        mov r13, r0
        msr cpsr_c, #0x13
        stmia r2, {r13}^
        movw r1, #0x0002
        movt r1, #0xBBBB
        str r1, [r2]
        ldmia r2, {r13}^
        mov r3, r13
        msr cpsr_c, #0x10
        mov r4, r13
        msr cpsr_c, #0x13
        """,
        [],
    ),
    (
        # CPS: the interrupt masks set and cleared, and the mode form.
        "cps_forms",
        0x1000,
        """
        cpsid if
        mrs r0, cpsr
        cpsie i
        mrs r1, cpsr
        cps #0x13
        mrs r2, cpsr
        """,
        [],
    ),
]

# bx_arm is assembled by the test itself: it places UDF at 0x200000 (VRAM)
# and jumps there, proving a cross-region branch through storage memory.


def canon_checks(ks):
    """Fail loudly if keystone is not encoding plain ARM as expected."""
    expected = {
        "mov r0, #5": 0xE3A00005,
        "adds r1, r1, #1": 0xE2911001,
        "ldr r0, [pc, #8]": 0xE59F0008,
        "str r1, [r0]": 0xE5801000,
        "strh r1, [r0]": 0xE1C010B0,
        "ldrsb r3, [r0, #1]": 0xE1D030D1,
        "ldrsh r4, [r0, #2]": 0xE1D040F2,
        "mrs r10, cpsr": 0xE10FA000,
        "msr cpsr_c, #0x10": 0xE321F010,
        "msr cpsr_c, r0": 0xE121F000,
        "bx r0": 0xE12FFF10,
        "clz r1, r0": 0xE16F1F10,
        "mul r2, r0, r1": 0xE0020190,
        "svc #0x80": 0xEF000080,
        "mov r0, #0x20000000": 0xE3A00202,
        "stmdb r11!, {r0-r2}": 0xE92B0007,
        "ldmia r11, {r3-r5}": 0xE89B0038,
        "mov r0, #0x200000": 0xE3A00602,
    }
    for asm, want in expected.items():
        enc, _ = ks.asm(asm)
        got = struct.unpack("<I", bytes(enc[:4]))[0]
        if got != want:
            sys.stderr.write(f"keystone encoding drift: {asm!r}: {got:08X} != {want:08X}\n")
            sys.exit(1)


def assemble_program(ks, name, org, body, raws):
    if body.strip():
        enc, _ = ks.asm(body, org)
        if len(enc) % 4:
            sys.stderr.write(f"{name}: assembled size not a multiple of 4\n")
            sys.exit(1)
        words = [struct.unpack("<I", bytes(enc[i : i + 4]))[0] for i in range(0, len(enc), 4)]
    else:
        words = []
    words += raws
    words.append(UDF)
    return words


def main():
    ks = Ks(KS_ARCH_ARM, KS_MODE_ARM)
    canon_checks(ks)

    out = []
    out.append("// Generated by scripts/gen_arm_tests.py -- do not edit by hand.")
    out.append("// ARMv7 test programs for the stage-3 interpreter, assembled from real")
    out.append("// assembly by keystone so every encoding is checked against an")
    out.append("// independent assembler. Programs end with UDF (kUnknownInstruction),")
    out.append("// which the tests treat as a clean end of program.")
    out.append("#pragma once")
    out.append("#include <stddef.h>")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("struct ArmProg {")
    out.append("    const char* name;")
    out.append("    uint32_t org;")
    out.append("    const uint32_t* words;")
    out.append("    size_t n_words;")
    out.append("    const char* asm_text;")
    out.append("};")
    out.append("")

    tables = []
    for name, org, body, raws in PROGRAMS:
        words = assemble_program(ks, name, org, body, raws)
        text = " | ".join(
            ln.strip()
            for ln in body.strip().splitlines()
            if ln.strip() and not ln.strip().startswith("@")
        )
        if raws:
            text += " | RAW: " + ", ".join(f"0x{w:08X}" for w in raws)
        sym = f"prog_words_{name}"
        tables.append(sym)
        out.append(f"static const uint32_t {sym}[] = {{")
        for i in range(0, len(words), 4):
            chunk = ", ".join(f"0x{w:08X}" for w in words[i : i + 4])
            out.append(f"    {chunk},")
        out.append("};")
        out.append("")
        out.append(f'static const char prog_text_{name}[] = "{text}";')
        out.append("")

    out.append("static const ArmProg kArmProgs[] = {")
    for (name, org, _, _), sym in zip(PROGRAMS, tables):
        out.append(f'    {{"{name}", 0x{org:X}, {sym},')
        out.append(f"     sizeof({sym}) / sizeof({sym}[0]), prog_text_{name}}},")
    out.append("};")
    out.append(f"static const size_t kArmProgCount = {len(PROGRAMS)};")
    out.append("")

    result = "\n".join(out) + "\n"
    dest = os.path.join(os.path.dirname(__file__), "..", "tests", "gen", "arm_progs.h")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if "--check" in sys.argv:
        with open(dest) as f:
            if f.read() != result:
                sys.stderr.write("tests/gen/arm_progs.h is stale; regenerate\n")
                return 1
        return 0
    with open(dest, "w") as f:
        f.write(result)
    print(f"wrote {os.path.normpath(dest)}: {len(PROGRAMS)} programs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
