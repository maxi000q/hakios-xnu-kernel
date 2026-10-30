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
