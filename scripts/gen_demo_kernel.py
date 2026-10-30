#!/usr/bin/env python3
"""Generate the bundled self-test kernel: app/src/main/assets/demo-kernel.img3.

The kernel the app can boot with no Apple code and no keys -- and the one the
desktop rig runs to prove stages 2-6 together (tests/test_demo_kernel.cpp).
It is a real armv7 program, assembled here with the keystone engine into the
instruction set the stage-3 interpreter implements, wrapped in the full
stage-4 chain (Mach-O -> 'comp'/LZSS -> IMG3), loaded by the real loader,
run by the real CPU against the real storage image, drawn through the real
VRAM map, and halted with the engine's clean power-off (SVC #0x0FF).

What it does, in order, all checkable:
  1. fills 64 KiB of guest RAM at 0x80040000 with a pattern and reads it
     back, counting words that came back wrong (r11 = the error count);
  2. paints the 640x960 RGB565 framebuffer at 0x20000000: each row y gets
     the color 0x801F | ((y & 0x3F) << 5) (bit 15, a green ramp by row, a
     blue rim) -- every pixel of every row;
  3. writes the self-test marker to the top of VRAM: 0x3A1D5C5E at
     VRAM+0, the error count at VRAM+4;
  4. powers off cleanly: SVC #0x0FF, the engine's "finished on purpose".

Regenerate after changing the assembly or the formats (keystone needed):

    pip install keystone-engine
    python3 scripts/gen_demo_kernel.py && tests/run_desktop_tests.sh
"""
import os
import sys

import keystone

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from kernelbuilder import comp_wrap, img3_wrap, macho_armv7_kernel  # noqa: E402

OUT = os.path.join(HERE, '..', 'app', 'src', 'main', 'assets', 'demo-kernel.img3')

# The guest constants the kernel and the tests agree on
# (tests/test_demo_kernel.cpp mirrors these by name).
VRAM = 0x20000000
SCREEN_W = 640
SCREEN_H = 960
TEST_AREA = 0x80040000
TEST_WORDS = 64 * 1024 // 4          # 64 KiB of u32s
PATTERN = 0x600DF00D                 # a marker that is also data
MARKER = 0x3A1D5C5E                  # the self-test's "I finished" word
POWEROFF_SVC = 0x0FF


def arm_const(n):
    """Decompose n into ARM-encodable rotated immediates (an 8-bit value
    rotated right by an even amount), largest first -- the generator emits a
    mov/add chain from it, so no hand-rolled constant math can drift."""
    parts = []
    rest = n & 0xFFFFFFFF
    while rest:
        best = 0
        for rot in range(0, 32, 2):
            for byte in range(1, 256):
                v = byte if rot == 0 else (
                    (byte >> rot) | (byte << (32 - rot))) & 0xFFFFFFFF
                if v <= rest and v > best:
                    best = v
        if best == 0:
            raise ValueError('0x%08x cannot be built from ARM immediates' % n)
        parts.append(best)
        rest = (rest - best) & 0xFFFFFFFF
    return parts


def const_asm(reg, n):
    parts = arm_const(n)
    lines = ['    mov  %s, #0x%X' % (reg, parts[0])]
    for p in parts[1:]:
        lines.append('    add  %s, %s, #0x%X' % (reg, reg, p))
    return '\n'.join(lines)


ASM = """start:
    @ ---- 1. the memory test: fill, then read back and count errors -------
{const_test}
    mov  r1, #0
{const_pattern}
fill:
    str  r3, [r0], #4
    add  r1, r1, #1
    cmp  r1, #{TEST_WORDS}
    blt  fill
{const_test}
    mov  r1, #0
    mov  r11, #0                      @ the error count
verify:
    ldr  r4, [r0], #4
    cmp  r4, r3
    addeq r1, r1, #1
    addne r11, r11, #1
    cmp  r1, #{TEST_WORDS}
    blt  verify

    @ ---- 2. the framebuffer: every row, every pixel -----------------------
    mov  r9, #0x20000000              @ VRAM base (encodable as-is)
    mov  r0, #0                       @ y
    mov  r6, #{SCREEN_W}
    mov  r7, #{SCREEN_H}
row:
    mov  r1, #0                       @ x
    and  r2, r0, #0x3F                @ y & 63
    lsl  r2, r2, #5                   @ the green ramp, RGB565 bits 10..5
    orr  r2, r2, #0x8000              @ bit 15 set
    orr  r2, r2, #0x1F                @ and the low blue rim
col:
    strh r2, [r9], #2
    add  r1, r1, #1
    cmp  r1, r6
    blt  col
    add  r0, r0, #1
    cmp  r0, r7
    blt  row

    @ ---- 3. the self-test marker at the top of VRAM ------------------------
    mov  r9, #0x20000000
{const_marker}
    str  r3, [r9]
    str  r11, [r9, #4]                @ the error count follows the marker

    @ ---- 4. the clean power-off ---------------------------------------------
    svc  #0x{POWEROFF_SVC:X}
    b    start                         @ never reached; the engine stops at the SVC
"""


def main():
    ks = keystone.Ks(keystone.KS_ARCH_ARM, keystone.KS_MODE_ARM)
    asm = ASM.format(const_test=const_asm('r0', TEST_AREA),
                     const_pattern=const_asm('r3', PATTERN),
                     const_marker=const_asm('r3', MARKER),
                     TEST_WORDS=TEST_WORDS, SCREEN_W=SCREEN_W, SCREEN_H=SCREEN_H,
                     POWEROFF_SVC=POWEROFF_SVC)
    code, _ = ks.asm(asm, addr=0x80001000)
    code = bytes(code)
    print('assembled: %d bytes of armv7' % len(code))

    macho = macho_armv7_kernel(code)
    img3 = img3_wrap(comp_wrap(macho))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'wb') as f:
        f.write(img3)
    print('wrote %s: macho=%d img3=%d' % (os.path.relpath(OUT, HERE + '/..'),
                                          len(macho), len(img3)))
    print('agreed facts: vram=0x%08x %dx%d test-area=0x%08x words=%d '
          'pattern=0x%08x marker=0x%08x poweroff-svc=0x%X'
          % (VRAM, SCREEN_W, SCREEN_H, TEST_AREA, TEST_WORDS, PATTERN, MARKER,
             POWEROFF_SVC))


if __name__ == '__main__':
    main()
