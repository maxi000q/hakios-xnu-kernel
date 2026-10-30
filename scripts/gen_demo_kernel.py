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
     back, counting words that came back wrong (saved for the report);
  2. paints a boot show on the 640x960 RGB565 framebuffer at 0x20000000:
     black screen, the HakiOS roundel logo, the wordmark, a version line,
     and a progress bar that fills step by step on real delay loops --
     every pixel through the same fill/text routines, at the 30 fps the
     renderer paces;
  3. paints the system screen: a vertical gradient, a title strip, the
     self-test report lines (memory, display, loader, CPU, the error count
     in decimal) and a 3-2-1 power-off countdown, erasing and redrawing
     the countdown region like a real firmware would;
  4. writes the self-test marker to the top of VRAM: 0x3A1D5C5E at
     VRAM+0, the error count at VRAM+4;
  5. powers off cleanly: SVC #0x0FF, the engine's "finished on purpose".

The drawing primitives (fill_rect, draw_char, draw_text, draw_dec,
draw_logo) are real ARM routines with one calling convention: args in
r0-r3/r12, every register preserved, stack allowed (the loader's
LC_UNIXTHREAD sets a usable SP inside guest RAM). Glyphs are a 5x7 font,
the logo a 1-bpp bitmap; both ride in the Mach-O __DATA segment the real
loader places in guest RAM, next to the code.

Regenerate after changing the assembly or the formats (keystone needed):

    pip install keystone-engine
    python3 scripts/gen_demo_kernel.py && tests/run_desktop_tests.sh

And to eyeball what the kernel paints (writes the framebuffer as PPM):

    tests/dump_demo_screen.sh
"""
import os
import re
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

# The Mach-O the builder emits: __TEXT at 0x80001000, __DATA right after
# at +0x10000, a stack for the routines, and the fonts/bitmap/strings in
# __DATA -- the real loader places all of it in guest RAM.
TEXT_BASE = 0x80001000
DATA_BASE = TEXT_BASE + 0x10000
STACK_TOP = 0x81000000               # inside guest RAM, above every area used
CLEAR_WORDS = SCREEN_W * SCREEN_H // 2   # u32 writes clear 2 pixels each

# -- the show's palette (RGB565) --------------------------------------------
def rgb565(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3

WHITE = rgb565(255, 255, 255)        # 0xFFFF
GRAY = rgb565(200, 200, 200)
GREEN = rgb565(0, 210, 60)
RED = rgb565(240, 60, 60)
BAR_FILL = rgb565(0, 122, 255)       # the progress bar's blue
BAR_TRACK = rgb565(60, 60, 66)       # the bar's empty track
STRIP = rgb565(8, 40, 110)           # the title strip
# The gradient interpolates channel by channel (adding raw RGB565 values
# bleeds bits between channels as blue wraps -- stripes, not a ramp). Each
# channel walks its own 16.16 fixed-point step; the steps live in __DATA.
GRAD_TR, GRAD_TG, GRAD_TB = 20 >> 3, 90 >> 2, 200 >> 3      # top row 2,22,25
GRAD_BR, GRAD_BG, GRAD_BB = 90 >> 3, 170 >> 2, 255 >> 3     # bottom 11,42,31
GRAD_STEP_R = (((GRAD_BR - GRAD_TR) << 16) + SCREEN_H // 2) // SCREEN_H
GRAD_STEP_G = (((GRAD_BG - GRAD_TG) << 16) + SCREEN_H // 2) // SCREEN_H
GRAD_STEP_B = (((GRAD_BB - GRAD_TB) << 16) + SCREEN_H // 2) // SCREEN_H

# -- the boot layout (mirrored by the test's `agreed` namespace) -------------
LOGO_X, LOGO_Y, LOGO_SCALE = 224, 170, 3      # 64x64 bitmap -> 192x192
WORDMARK_X, WORDMARK_Y, WORDMARK_SCALE = 215, 410, 6
VERSION_X, VERSION_Y, VERSION_SCALE = 261, 480, 2
SELFTEST_X, SELFTEST_Y, SELFTEST_SCALE = 243, 612, 2
BAR_X, BAR_Y, BAR_W, BAR_H = 140, 560, 364, 14
BAR_IN_X, BAR_IN_Y, BAR_IN_W, BAR_IN_H = 142, 562, 360, 10
BAR_STEPS = 60                                 # 60 * 6px = the inner width
TITLE_X, TITLE_Y, TITLE_SCALE = 20, 14, 2
LINE_X, LINE_SCALE = 40, 2
LINE_Y0, LINE_DY = 120, 44
OK_X = 500
COUNT_X, COUNT_Y, COUNT_SCALE = 40, 876, 2
COUNT_DIGIT_X = 200
COUNT_ERASE_Y, COUNT_ERASE_H = 860, 60

# The pacing: pure delay loops (subs/bne), sized so the whole show runs a
# couple of seconds on a phone's big core and stays a sub-second detour in
# the desktop rig. 2 instructions per iteration.
BAR_DELAY_ITERS = 1_000_000         # ~2M instructions per bar step
COUNT_DELAY_ITERS = 6_000_000       # ~12M instructions per countdown digit

# -- the 5x7 font: 7 rows per glyph, bit4..bit0 = column 0..4 ----------------
FONT_GLYPHS = {
    ' ': ['00000'] * 7,
    '.': ['00000', '00000', '00000', '00000', '00000', '01100', '01100'],
    '-': ['00000', '00000', '00000', '11111', '00000', '00000', '00000'],
    '/': ['00001', '00010', '00010', '00100', '01000', '01000', '10000'],
    ':': ['00000', '01100', '01100', '00000', '01100', '01100', '00000'],
    '0': ['01110', '10001', '10011', '10101', '11001', '10001', '01110'],
    '1': ['00100', '01100', '00100', '00100', '00100', '00100', '01110'],
    '2': ['01110', '10001', '00001', '00010', '00100', '01000', '11111'],
    '3': ['11111', '00010', '00100', '00010', '00001', '10001', '01110'],
    '4': ['00010', '00110', '01010', '10010', '11111', '00010', '00010'],
    '5': ['11111', '10000', '11110', '00001', '00001', '10001', '01110'],
    '6': ['00110', '01000', '10000', '11110', '10001', '10001', '01110'],
    '7': ['11111', '00001', '00010', '00100', '01000', '01000', '01000'],
    '8': ['01110', '10001', '10001', '01110', '10001', '10001', '01110'],
    '9': ['01110', '10001', '10001', '01111', '00001', '00010', '01100'],
    'A': ['01110', '10001', '10001', '11111', '10001', '10001', '10001'],
    'B': ['11110', '10001', '10001', '11110', '10001', '10001', '11110'],
    'C': ['01110', '10001', '10000', '10000', '10000', '10001', '01110'],
    'D': ['11110', '10001', '10001', '10001', '10001', '10001', '11110'],
    'E': ['11111', '10000', '10000', '11110', '10000', '10000', '11111'],
    'F': ['11111', '10000', '10000', '11110', '10000', '10000', '10000'],
    'G': ['01110', '10001', '10000', '10111', '10001', '10001', '01111'],
    'H': ['10001', '10001', '10001', '11111', '10001', '10001', '10001'],
    'I': ['01110', '00100', '00100', '00100', '00100', '00100', '01110'],
    'J': ['00111', '00010', '00010', '00010', '00010', '10010', '01100'],
    'K': ['10001', '10010', '10100', '11000', '10100', '10010', '10001'],
    'L': ['10000', '10000', '10000', '10000', '10000', '10000', '11111'],
    'M': ['10001', '11011', '10101', '10101', '10001', '10001', '10001'],
    'N': ['10001', '10001', '11001', '10101', '10011', '10001', '10001'],
    'O': ['01110', '10001', '10001', '10001', '10001', '10001', '01110'],
    'P': ['11110', '10001', '10001', '11110', '10000', '10000', '10000'],
    'Q': ['01110', '10001', '10001', '10001', '10101', '10010', '01101'],
    'R': ['11110', '10001', '10001', '11110', '10100', '10010', '10001'],
    'S': ['01111', '10000', '10000', '01110', '00001', '00001', '11110'],
    'T': ['11111', '00100', '00100', '00100', '00100', '00100', '00100'],
    'U': ['10001', '10001', '10001', '10001', '10001', '10001', '01110'],
    'V': ['10001', '10001', '10001', '10001', '10001', '01010', '00100'],
    'W': ['10001', '10001', '10001', '10101', '10101', '11011', '10001'],
    'X': ['10001', '10001', '01010', '00100', '01010', '10001', '10001'],
    'Y': ['10001', '10001', '01010', '00100', '00100', '00100', '00100'],
    'Z': ['11111', '00001', '00010', '00100', '01000', '10000', '11111'],
}
FONT_FIRST = 32                     # ASCII space
FONT_LAST = 95                      # ASCII underscore; only what we define
FONT_WORDS_PER_GLYPH = 7

# The strings the kernel draws (uppercase only -- the font is uppercase).
STRINGS = [
    'HAKIOS',            # the wordmark
    'KERNEL 6.1',
    'SELF-TEST BOOT',
    'HAKIOS 6.1',        # the system screen's title strip
    'MEMORY 64K',
    'DISPLAY 640X960',
    'LOADER IMG3',
    'CPU ARMV7',
    'ERRORS',
    'OK',
    'POWER OFF IN',
]

# -- __DATA layout: font, powers-of-10 table, gradient steps, strings, logo --
FONT_ADDR = DATA_BASE
POWERS_ADDR = FONT_ADDR + (FONT_LAST - FONT_FIRST + 1) * FONT_WORDS_PER_GLYPH * 4
STEPS_ADDR = POWERS_ADDR + 11 * 4
STRINGS_ADDR = STEPS_ADDR + 3 * 4
str_addr = {}
cursor = STRINGS_ADDR
for s in STRINGS:
    str_addr[s] = cursor
    cursor += len(s) + 1
LOGO_ADDR = (cursor + 3) & ~3
LOGO_W, LOGO_H = 64, 64
DATA_END = LOGO_ADDR + LOGO_W * LOGO_H // 8 * 4 // 4 * 4  # 2 words per row
DATA_BYTES = LOGO_ADDR + (LOGO_H * 2 * 4) - DATA_BASE
assert DATA_BYTES <= 0x1000, 'the __DATA segment is 4 KiB, have %d' % DATA_BYTES

# Scratch inside RAM: the saved error count (and nothing else).
SCRATCH = 0x80060000


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


def font_words():
    out = []
    for code in range(FONT_FIRST, FONT_LAST + 1):
        g = FONT_GLYPHS.get(chr(code), ['00000'] * 7)
        assert len(g) == 7 and all(len(r) == 5 for r in g), chr(code)
        out.extend(int(row, 2) for row in g)
    return out


def logo_words():
    """The roundel: a ring plus a bold H, 64x64, bit31 of each word = the
    leftmost pixel of its 32-pixel half-row."""
    import math
    out = []
    for y in range(LOGO_H):
        words = [0, 0]
        for x in range(LOGO_W):
            dx, dy = x - 31.5, y - 31.5
            d = math.sqrt(dx * dx + dy * dy)
            ring = 26.0 <= d <= 30.0
            h_bar = (18 <= x <= 24 or 40 <= x <= 46) and 18 <= y <= 45
            h_cross = 18 <= x <= 46 and 29 <= y <= 35
            if ring or h_bar or h_cross:
                words[x >> 5] |= 1 << (31 - (x & 31))
        out.extend(words)
    return out


def data_words():
    """The __DATA segment's words: font, powers of ten, the string blob,
    then the logo bitmap -- addresses exactly as the layout above computed
    them (strings are NUL-terminated and packed back to back; the total is
    padded to a word boundary before the aligned logo)."""
    words = list(font_words())
    words.extend([10 ** k for k in range(9, -1, -1)] + [0])
    words.extend([GRAD_STEP_R, GRAD_STEP_G, GRAD_STEP_B])
    blob = b''.join(s.encode('ascii') + b'\0' for s in STRINGS)
    blob += b'\0' * ((-len(blob)) % 4)
    words.extend(int.from_bytes(blob[i:i + 4], 'little')
                 for i in range(0, len(blob), 4))
    pad_words = (LOGO_ADDR - DATA_BASE) // 4 - len(words)
    assert pad_words >= 0, 'the layout overran the logo'
    words.extend([0] * pad_words)
    words.extend(logo_words())
    return words


ASM = """start:
    @ ---- 0. the kernel's own stack ------------------------------------------
    @ The engine's CPU starts with a zeroed register file; the drawing
    @ routines below need a stack, so the kernel sets its own, the way a
    @ real one does, at the top of a RAM page nothing else uses.
    mov  sp, #0x81000000

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
{const_scratch}
    str  r11, [r0]                    @ keep it for the report lines

    @ ---- 2. the boot screen ----------------------------------------------
    mov  r9, #0x20000000              @ VRAM base, kept for the whole kernel
    mov  r0, #0
{const_r1_clear}
    mov  r4, r9                       @ the cursor; r9 stays the base
clr:
    str  r0, [r4], #4                 @ black, two pixels a word
    subs r1, r1, #1
    bne  clr

    @ the roundel, the wordmark, the version line
    mov  r0, #{LOGO_ADDR_IMM}
{const_logo_addr_rest}
    mov  r1, #{LOGO_X}
    mov  r2, #{LOGO_Y}
    {const_white_r3}
    mov  r12, #{LOGO_SCALE}
    bl   draw_logo
    mov  r0, #{STR_WORDMARK_IMM}
{const_wordmark_addr_rest}
    mov  r1, #{WORDMARK_X}
    mov  r2, #{WORDMARK_Y}
    {const_white_r3}
    mov  r12, #{WORDMARK_SCALE}
    bl   draw_text
    mov  r0, #{STR_VERSION_IMM}
{const_version_addr_rest}
    mov  r1, #{VERSION_X}
    mov  r2, #{VERSION_Y}
{const_r3_gray}
    mov  r12, #{VERSION_SCALE}
    bl   draw_text
    mov  r0, #{STR_SELFTEST_IMM}
{const_selftest_addr_rest}
    mov  r1, #{SELFTEST_X}
    mov  r2, #{SELFTEST_Y}
{const_r3_gray2}
    mov  r12, #{SELFTEST_SCALE}
    bl   draw_text

    @ the progress bar: outline, track, then 60 fill steps with a delay each
    mov  r0, #{BAR_X}
    mov  r1, #{BAR_Y}
    mov  r2, #{BAR_W}
    mov  r3, #2
    {const_white_r12}
    bl   fill_rect
    mov  r0, #{BAR_X}
    mov  r1, #{BAR_Y_BARBOT}
    mov  r2, #{BAR_W}
    mov  r3, #2
    {const_white_r12}
    bl   fill_rect
    mov  r0, #{BAR_X}
    mov  r1, #{BAR_Y}
    mov  r2, #2
    mov  r3, #{BAR_H}
    {const_white_r12}
    bl   fill_rect
    mov  r0, #{BAR_X_RIGHT}
    mov  r1, #{BAR_Y}
    mov  r2, #2
    mov  r3, #{BAR_H}
    {const_white_r12}
    bl   fill_rect
    mov  r0, #{BAR_IN_X}
    mov  r1, #{BAR_IN_Y}
    mov  r2, #{BAR_IN_W}
    mov  r3, #{BAR_IN_H}
{const_r12_track}
    bl   fill_rect
    mov  r10, #0                      @ the step counter
bar_step:
    mov  r0, r10
    mov  r1, #6
    mul  r0, r0, r1
    add  r0, r0, #{BAR_IN_X}
    mov  r1, #{BAR_IN_Y}
    mov  r2, #6
    mov  r3, #{BAR_IN_H}
{const_r12_fill}
    bl   fill_rect
{const_r0_bar_delay}
    bl   delay
    add  r10, r10, #1
    cmp  r10, #{BAR_STEPS}
    blt  bar_step

    @ ---- 3. the system screen --------------------------------------------
    @ the gradient: three 16.16 channels, one step each a row, two pixels
    @ a word -- channel-wise, so no bit ever bleeds between colors
{const_r2_steps}
    ldr  r3, [r2, #0]                 @ the r, g and b steps
    ldr  r12, [r2, #4]
    ldr  r2, [r2, #8]
{const_r4_acc_r}
{const_r5_acc_g}
{const_r6_acc_b}
    mov  r7, #0                       @ y
grad_row:
    mov  r11, r4, lsr #16
    and  r11, r11, #0x1F
    mov  r8, r11, lsl #11             @ r lives in the top five bits
    mov  r11, r5, lsr #16
    and  r11, r11, #0x3F
    orr  r8, r8, r11, lsl #5          @ g in the middle six
    mov  r11, r6, lsr #16
    and  r11, r11, #0x1F
    orr  r8, r8, r11                  @ b in the low five
    orr  r8, r8, r8, lsl #16          @ ... twice, for one u32 store
    mov  r0, r7
    mov  r1, #1280
    mul  r0, r0, r1
    add  r0, r9, r0                   @ the row's first pixel
    mov  r1, #320
grad_fill:
    str  r8, [r0], #4
    subs r1, r1, #1
    bne  grad_fill
    add  r4, r4, r3
    add  r5, r5, r12
    add  r6, r6, r2
    add  r7, r7, #1
    cmp  r7, #{SCREEN_H}
    blt  grad_row

    @ the title strip and the title
    mov  r0, #0
    mov  r1, #0
    mov  r2, #{SCREEN_W}
    mov  r3, #44
{const_r12_strip}
    bl   fill_rect
    mov  r0, #{STR_TITLE_IMM}
{const_title_addr_rest}
    mov  r1, #{TITLE_X}
    mov  r2, #{TITLE_Y}
    {const_white_r3}
    mov  r12, #{TITLE_SCALE}
    bl   draw_text

    @ the report lines: the fact on the left, the verdict on the right
    mov  r0, #{STR_MEMORY_IMM}
{const_memory_addr_rest}
    mov  r1, #{LINE_X}
    mov  r2, #{LINE_Y0}
    {const_white_r3}
    mov  r12, #{LINE_SCALE}
    bl   draw_text
    mov  r0, #{STR_OK_IMM}
{const_ok_addr_rest}
    mov  r1, #{OK_X}
    mov  r2, #{LINE_Y0}
{const_r3_green}
    mov  r12, #{LINE_SCALE}
    bl   draw_text
    mov  r0, #{STR_DISPLAY_IMM}
{const_display_addr_rest}
    mov  r1, #{LINE_X}
    mov  r2, #{LINE_Y1}
    {const_white_r3}
    mov  r12, #{LINE_SCALE}
    bl   draw_text
    mov  r0, #{STR_OK_IMM2}
{const_ok_addr_rest2}
    mov  r1, #{OK_X}
    mov  r2, #{LINE_Y1}
{const_r3_green2}
    mov  r12, #{LINE_SCALE}
    bl   draw_text
    mov  r0, #{STR_LOADER_IMM}
{const_loader_addr_rest}
    mov  r1, #{LINE_X}
    mov  r2, #{LINE_Y2}
    {const_white_r3}
    mov  r12, #{LINE_SCALE}
    bl   draw_text
    mov  r0, #{STR_OK_IMM3}
{const_ok_addr_rest3}
    mov  r1, #{OK_X}
    mov  r2, #{LINE_Y2}
{const_r3_green3}
    mov  r12, #{LINE_SCALE}
    bl   draw_text
    mov  r0, #{STR_CPU_IMM}
{const_cpu_addr_rest}
    mov  r1, #{LINE_X}
    mov  r2, #{LINE_Y3}
    {const_white_r3}
    mov  r12, #{LINE_SCALE}
    bl   draw_text
    mov  r0, #{STR_OK_IMM4}
{const_ok_addr_rest4}
    mov  r1, #{OK_X}
    mov  r2, #{LINE_Y3}
{const_r3_green4}
    mov  r12, #{LINE_SCALE}
    bl   draw_text
    mov  r0, #{STR_ERRORS_IMM}
{const_errors_addr_rest}
    mov  r1, #{LINE_X}
    mov  r2, #{LINE_Y4}
    {const_white_r3}
    mov  r12, #{LINE_SCALE}
    bl   draw_text
{const_scratch_r0}
    ldr  r0, [r0]                     @ the error count, back from RAM
    mov  r1, #{OK_X}
    mov  r2, #{LINE_Y4}
{const_white_r3}                      @ white when clean...
    cmp  r0, #0
    beq  errors_color
{const_r3_red}                        @ ...red when anything came back wrong
errors_color:
    mov  r12, #{LINE_SCALE}
    bl   draw_dec

    @ the countdown: erase the region, redraw the line and the digit
    mov  r10, #3
countdown:
    mov  r0, #0
    mov  r1, #{COUNT_ERASE_Y}
    mov  r2, #{SCREEN_W}
    mov  r3, #{COUNT_ERASE_H}
{const_r12_erase}
    bl   fill_rect
    mov  r0, #{STR_COUNT_IMM}
{const_count_addr_rest}
    mov  r1, #{COUNT_X}
    mov  r2, #{COUNT_Y}
    {const_white_r3}
    mov  r12, #{COUNT_SCALE}
    bl   draw_text
    mov  r0, r10
    mov  r1, #{COUNT_DIGIT_X}
    mov  r2, #{COUNT_Y}
    {const_white_r3}
    mov  r12, #{COUNT_SCALE}
    bl   draw_dec
{const_r0_count_delay}
    bl   delay
    subs r10, r10, #1
    bne  countdown

    @ ---- 4. the self-test marker at the top of VRAM ------------------------
    {const_marker}
    str  r3, [r9]
{const_scratch_r1}
    ldr  r1, [r1]
    str  r1, [r9, #4]                 @ the error count follows the marker

    @ ---- 5. the clean power-off ---------------------------------------------
    svc  #0x{POWEROFF_SVC:X}
    b    start                         @ never reached; the engine stops at the SVC

@ ---- the drawing primitives -------------------------------------------------
@ Convention for every routine: arguments in r0-r3 and r12, every register
@ preserved (the stack saves what the body uses), r9 is the VRAM base and is
@ never clobbered. The loader's LC_UNIXTHREAD gave the kernel a usable SP
@ inside guest RAM; each frame is 14 words.

fill_rect:                              @ r0=x r1=y r2=w r3=h r12=color
    stmfd sp!, {{r0-r12, lr}}
    mov  r4, #640
    mul  r4, r1, r4                     @ y*640
    add  r4, r4, r0                     @ +x
    mov  r4, r4, lsl #1                 @ *2: the byte offset of (x,y)
    add  r4, r9, r4                     @ the first row's address
    mov  r5, #1280                      @ a row is 1280 bytes
fr_row:
    mov  r6, r2                         @ pixels left in this row
    mov  r7, r4
fr_col:
    strh r12, [r7], #2
    subs r6, r6, #1
    bne  fr_col
    add  r4, r4, #1280
    subs r3, r3, #1
    bne  fr_row
    ldmfd sp!, {{r0-r12, pc}}

draw_char:                              @ r0=ch r1=x r2=y r3=color r12=scale
    stmfd sp!, {{r0-r12, lr}}
    mov  r4, r1                         @ x
    mov  r5, r2                         @ y
    mov  r6, r3                         @ color
    mov  r7, r12                        @ scale
    sub  r8, r0, #32
    mov  r0, #28
    mul  r8, r8, r0                     @ (ch-32)*28: the glyph's byte offset
{const_r0_font}
    add  r8, r8, r0                     @ the glyph's address in __DATA
    mov  r10, #0                        @ row
dc_row:
    cmp  r10, #7
    bge  dc_done
    mov  r11, #0                        @ col
dc_col:
    cmp  r11, #5
    bge  dc_next_row
    ldr  r0, [r8, r10, lsl #2]          @ the row's five bits
    rsb  r1, r11, #4
    mov  r2, #1
    mov  r2, r2, lsl r1                 @ the col's mask
    tst  r0, r2
    beq  dc_next_col
    mul  r0, r11, r7                    @ col*scale
    add  r0, r0, r4                     @ +x
    mul  r1, r10, r7                    @ row*scale
    add  r1, r1, r5                     @ +y
    mov  r2, r7                         @ a scale-square of...
    mov  r3, r7
    mov  r12, r6                        @ ...the glyph's color
    bl   fill_rect
dc_next_col:
    add  r11, r11, #1
    b    dc_col
dc_next_row:
    add  r10, r10, #1
    b    dc_row
dc_done:
    ldmfd sp!, {{r0-r12, pc}}

draw_text:                              @ r0=str r1=x r2=y r3=color r12=scale
    stmfd sp!, {{r0-r12, lr}}
    mov  r4, r0                         @ the string, NUL-terminated
    mov  r5, r1                         @ the x cursor
    mov  r6, r2
    mov  r7, r3
    mov  r8, r12
dt_loop:
    ldrb r0, [r4], #1
    cmp  r0, #0
    beq  dt_done
    mov  r1, r5
    mov  r2, r6
    mov  r3, r7
    mov  r12, r8
    bl   draw_char
    mov  r11, #6
    mul  r11, r11, r8                   @ advance: 6 pixels of glyph box
    add  r5, r5, r11
    b    dt_loop
dt_done:
    ldmfd sp!, {{r0-r12, pc}}

draw_logo:                              @ r0=bmp r1=x r2=y r3=color r12=scale
    stmfd sp!, {{r0-r12, lr}}           @ the bitmap is 64x64, 2 words a row
    mov  r4, r0
    mov  r5, r1
    mov  r6, r2
    mov  r7, r3
    mov  r8, r12
    mov  r10, #0                        @ row
dl_row:
    cmp  r10, #64
    bge  dl_done
    mov  r11, #0                        @ col
dl_col:
    cmp  r11, #64
    bge  dl_next_row
    mov  r0, r10, lsl #3                @ row*8: two words of bitmap
    add  r0, r4, r0
    mov  r1, r11, lsr #5                @ which of the row's two words
    add  r0, r0, r1, lsl #2
    ldr  r2, [r0]
    and  r3, r11, #31
    rsb  r3, r3, #31                    @ bit31 is the leftmost pixel
    mov  r1, #1
    mov  r1, r1, lsl r3
    tst  r2, r1
    beq  dl_next_col
    mul  r0, r11, r8
    add  r0, r0, r5
    mul  r1, r10, r8
    add  r1, r1, r6
    mov  r2, r8
    mov  r3, r8
    mov  r12, r7
    bl   fill_rect
dl_next_col:
    add  r11, r11, #1
    b    dl_col
dl_next_row:
    add  r10, r10, #1
    b    dl_row
dl_done:
    ldmfd sp!, {{r0-r12, pc}}

draw_dec:                               @ r0=value r1=x r2=y r3=color r12=scale
    stmfd sp!, {{r0-r12, lr}}           @ decimal, by powers of ten
    mov  r4, r0                         @ the value
    mov  r5, r1                         @ x
    mov  r6, r2
    mov  r7, r3
    mov  r8, r12
    mov  r10, #0                        @ digits emitted so far
{const_r11_powers}
dd_loop:
    ldr  r0, [r11], #4                  @ the power; 0 ends the table
    cmp  r0, #0
    beq  dd_done
    mov  r1, #0                         @ the digit
dd_sub:
    cmp  r4, r0
    blt  dd_got
    sub  r4, r4, r0
    add  r1, r1, #1
    b    dd_sub
dd_got:
    cmp  r1, #0                         @ skip a leading zero...
    bne  dd_emit
    cmp  r10, #0                        @ ...unless something was emitted...
    bne  dd_emit
    cmp  r0, #1                         @ ...or it is the last power
    beq  dd_emit
    b    dd_next
dd_emit:
    add  r1, r1, #48                    @ '0'
    mov  r0, r1
    mov  r1, #6
    mul  r1, r1, r8
    mul  r1, r1, r10                    @ the digit's x offset
    add  r1, r1, r5
    mov  r2, r6
    mov  r3, r7
    mov  r12, r8
    bl   draw_char
    add  r10, r10, #1
dd_next:
    b    dd_loop
dd_done:
    ldmfd sp!, {{r0-r12, pc}}

delay:                                  @ r0=iterations; 2 instructions each
    subs r0, r0, #1
    bne  delay
    bx   lr
"""


def split_imm(value):
    """A constant that may or may not fit one mov immediate: return the
    single-instruction form when it does, else the first part for a mov."""
    parts = arm_const(value)
    return parts[0], parts[1:]


def const_rest(reg, value):
    """The add-chain after the first mov immediate (empty when one was enough)."""
    return '\n'.join('    add  %s, %s, #0x%X' % (reg, reg, p)
                     for p in arm_const(value)[1:])


def encodable(v):
    """True when v is one ARM rotated immediate (8 bits, even rotation)."""
    v &= 0xFFFFFFFF
    if v == 0:
        return True                      # the trivial all-zero immediate
    for rot in range(0, 32, 2):
        for byte in range(1, 256):
            r = byte if rot == 0 else (
                (byte >> rot) | (byte << (32 - rot))) & 0xFFFFFFFF
            if r == v:
                return True
    return False


MOV_IMM = re.compile(r'^(\s*)(mov|add)\s+(r\d+)(?:,\s*(r\d+))?,\s*#(0x[0-9A-Fa-f]+|\d+)\s*$')
OTHER_IMM = re.compile(r'^\s*(cmp|sub|rsb|and|orr|bic|tst)\b')


def fixup_immediates(asm):
    """Rewrite every mov/add whose immediate is not a single rotated ARM
    immediate into a mov/add chain, and refuse to emit any other data-
    processing instruction with an unencodable one. Keystone silently
    assembles `mov rN, #unencodable` as MOVW, and the interpreter (which has
    no MOVW) executes that word as RSC -- garbage in a register and pixels
    a screen away from where they belong, with no fault to say so. The
    chains from arm_const are the same trick the const_asm placeholders
    have always used, applied mechanically to the whole listing."""
    out = []
    for line in asm.split('\n'):
        m = MOV_IMM.match(line)
        if m:
            indent, op, rd, rn, num = m.groups()
            value = int(num, 0)
            if value == 0 or encodable(value):
                out.append(line)          # one instruction says it already
                continue
            parts = arm_const(value)
            if op == 'mov':
                out.append('%smov  %s, #0x%X' % (indent, rd, parts[0]))
                for p in parts[1:]:
                    out.append('%sadd  %s, %s, #0x%X' % (indent, rd, rd, p))
            else:
                out.append('%sadd  %s, %s, #0x%X' % (indent, rd, rn, parts[0]))
                for p in parts[1:]:
                    out.append('%sadd  %s, %s, #0x%X' % (indent, rd, rd, p))
            continue
        if OTHER_IMM.match(line) and '#' in line and '[' not in line:
            for num in re.findall(r'#(0x[0-9A-Fa-f]+|\d+)', line):
                if not encodable(int(num, 0)):
                    raise SystemExit('unencodable immediate: ' + line)
        out.append(line)
    return '\n'.join(out)


def forbid_movw(code):
    """The interpreter has no MOVW/MOVT: any in the assembled code means an
    immediate slipped past the fixup and would execute as something else
    entirely. Fail the generation instead of shipping it."""
    for i in range(0, len(code) - 3, 4):
        w = int.from_bytes(code[i:i + 4], 'little')
        if (w & 0x0FF00000) == 0x03000000 or (w & 0x0FF00000) == 0x03400000:
            raise SystemExit('MOVW/MOVT at 0x%x (word %08X): an immediate '
                             'escaped the fixup' % (TEXT_BASE + i, w))


def main():
    ks = keystone.Ks(keystone.KS_ARCH_ARM, keystone.KS_MODE_ARM)

    fmt = {
        'const_test': const_asm('r0', TEST_AREA),
        'const_pattern': const_asm('r3', PATTERN),
        'const_scratch': const_asm('r0', SCRATCH),
        'const_scratch_r0': const_asm('r0', SCRATCH),
        'const_scratch_r1': const_asm('r1', SCRATCH),
        'const_marker': const_asm('r3', MARKER),
        'const_r1_clear': const_asm('r1', CLEAR_WORDS),
        'LOGO_ADDR_IMM': arm_const(LOGO_ADDR)[0],
        'const_logo_addr_rest': const_rest('r0', LOGO_ADDR),
        'STR_WORDMARK_IMM': arm_const(str_addr['HAKIOS'])[0],
        'const_wordmark_addr_rest': const_rest('r0', str_addr['HAKIOS']),
        'STR_VERSION_IMM': arm_const(str_addr['KERNEL 6.1'])[0],
        'const_version_addr_rest': const_rest('r0', str_addr['KERNEL 6.1']),
        'STR_SELFTEST_IMM': arm_const(str_addr['SELF-TEST BOOT'])[0],
        'const_selftest_addr_rest': const_rest('r0', str_addr['SELF-TEST BOOT']),
        'STR_TITLE_IMM': arm_const(str_addr['HAKIOS 6.1'])[0],
        'const_title_addr_rest': const_rest('r0', str_addr['HAKIOS 6.1']),
        'STR_MEMORY_IMM': arm_const(str_addr['MEMORY 64K'])[0],
        'const_memory_addr_rest': const_rest('r0', str_addr['MEMORY 64K']),
        'STR_DISPLAY_IMM': arm_const(str_addr['DISPLAY 640X960'])[0],
        'const_display_addr_rest': const_rest('r0', str_addr['DISPLAY 640X960']),
        'STR_LOADER_IMM': arm_const(str_addr['LOADER IMG3'])[0],
        'const_loader_addr_rest': const_rest('r0', str_addr['LOADER IMG3']),
        'STR_CPU_IMM': arm_const(str_addr['CPU ARMV7'])[0],
        'const_cpu_addr_rest': const_rest('r0', str_addr['CPU ARMV7']),
        'STR_ERRORS_IMM': arm_const(str_addr['ERRORS'])[0],
        'const_errors_addr_rest': const_rest('r0', str_addr['ERRORS']),
        'STR_OK_IMM': arm_const(str_addr['OK'])[0],
        'const_ok_addr_rest': const_rest('r0', str_addr['OK']),
        'STR_OK_IMM2': arm_const(str_addr['OK'])[0],
        'const_ok_addr_rest2': const_rest('r0', str_addr['OK']),
        'STR_OK_IMM3': arm_const(str_addr['OK'])[0],
        'const_ok_addr_rest3': const_rest('r0', str_addr['OK']),
        'STR_OK_IMM4': arm_const(str_addr['OK'])[0],
        'const_ok_addr_rest4': const_rest('r0', str_addr['OK']),
        'STR_COUNT_IMM': arm_const(str_addr['POWER OFF IN'])[0],
        'const_count_addr_rest': const_rest('r0', str_addr['POWER OFF IN']),
        'const_r3_gray': const_asm('r3', GRAY),
        'const_r3_gray2': const_asm('r3', GRAY),
        'const_r3_green': const_asm('r3', GREEN),
        'const_r3_green2': const_asm('r3', GREEN),
        'const_r3_green3': const_asm('r3', GREEN),
        'const_r3_green4': const_asm('r3', GREEN),
        'const_r3_red': const_asm('r3', RED),
        'const_white_r3': const_asm('r3', WHITE),
        'const_white_r12': const_asm('r12', WHITE),
        'const_r12_track': const_asm('r12', BAR_TRACK),
        'const_r12_fill': const_asm('r12', BAR_FILL),
        'const_r12_strip': const_asm('r12', STRIP),
        'const_r12_erase': const_asm('r12', gradient_at(COUNT_ERASE_Y + 4)),
        'const_r2_steps': const_asm('r2', STEPS_ADDR),
        'const_r4_acc_r': const_asm('r4', GRAD_TR << 16),
        'const_r5_acc_g': const_asm('r5', GRAD_TG << 16),
        'const_r6_acc_b': const_asm('r6', GRAD_TB << 16),
        'const_r0_bar_delay': const_asm('r0', BAR_DELAY_ITERS),
        'const_r0_count_delay': const_asm('r0', COUNT_DELAY_ITERS),
        'const_r0_font': const_asm('r0', FONT_ADDR),
        'const_r11_powers': const_asm('r11', POWERS_ADDR),
        'TEST_WORDS': TEST_WORDS,
        'SCREEN_W': SCREEN_W,
        'SCREEN_H': SCREEN_H,
        'POWEROFF_SVC': POWEROFF_SVC,
        'LOGO_X': LOGO_X, 'LOGO_Y': LOGO_Y, 'LOGO_SCALE': LOGO_SCALE,
        'WORDMARK_X': WORDMARK_X, 'WORDMARK_Y': WORDMARK_Y,
        'WORDMARK_SCALE': WORDMARK_SCALE,
        'VERSION_X': VERSION_X, 'VERSION_Y': VERSION_Y,
        'VERSION_SCALE': VERSION_SCALE,
        'SELFTEST_X': SELFTEST_X, 'SELFTEST_Y': SELFTEST_Y,
        'SELFTEST_SCALE': SELFTEST_SCALE,
        'BAR_X': BAR_X, 'BAR_Y': BAR_Y, 'BAR_W': BAR_W, 'BAR_H': BAR_H,
        'BAR_Y_BARBOT': BAR_Y + BAR_H - 2,
        'BAR_X_RIGHT': BAR_X + BAR_W - 2,
        'BAR_IN_X': BAR_IN_X, 'BAR_IN_Y': BAR_IN_Y,
        'BAR_IN_W': BAR_IN_W, 'BAR_IN_H': BAR_IN_H,
        'BAR_STEPS': BAR_STEPS,
        'TITLE_X': TITLE_X, 'TITLE_Y': TITLE_Y, 'TITLE_SCALE': TITLE_SCALE,
        'LINE_X': LINE_X, 'LINE_SCALE': LINE_SCALE,
        'LINE_Y0': LINE_Y0,
        'LINE_Y1': LINE_Y0 + LINE_DY,
        'LINE_Y2': LINE_Y0 + 2 * LINE_DY,
        'LINE_Y3': LINE_Y0 + 3 * LINE_DY,
        'LINE_Y4': LINE_Y0 + 4 * LINE_DY,
        'OK_X': OK_X,
        'COUNT_X': COUNT_X, 'COUNT_Y': COUNT_Y, 'COUNT_SCALE': COUNT_SCALE,
        'COUNT_DIGIT_X': COUNT_DIGIT_X,
        'COUNT_ERASE_Y': COUNT_ERASE_Y, 'COUNT_ERASE_H': COUNT_ERASE_H,
    }
    asm = ASM.format(**fmt)
    asm = fixup_immediates(asm)
    code, _ = ks.asm(asm, addr=TEXT_BASE)
    code = bytes(code)
    forbid_movw(code)
    print('assembled: %d bytes of armv7' % len(code))

    macho = macho_armv7_kernel(code, data_words=data_words(),
                               text_vmaddr=TEXT_BASE, entry_sp=STACK_TOP)
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
    print('data: font=0x%08x powers=0x%08x steps=0x%08x strings=0x%08x '
          'logo=0x%08x (%d B)'
          % (FONT_ADDR, POWERS_ADDR, STEPS_ADDR, STRINGS_ADDR, LOGO_ADDR,
             DATA_BYTES))
    print('palette: white=0x%04X gray=0x%04X green=0x%04X red=0x%04X '
          'bar=0x%04X track=0x%04X strip=0x%04X'
          % (WHITE, GRAY, GREEN, RED, BAR_FILL, BAR_TRACK, STRIP))
    print('gradient: top=(%d,%d,%d) steps=(%d,%d,%d) (0x%X,0x%X,0x%X) '
          'erase=0x%04X'
          % (GRAD_TR, GRAD_TG, GRAD_TB, GRAD_STEP_R, GRAD_STEP_G, GRAD_STEP_B,
             GRAD_STEP_R, GRAD_STEP_G, GRAD_STEP_B,
             gradient_at(COUNT_ERASE_Y + 4)))


def gradient_at(y):
    """The gradient's row color, the way the kernel's fixed point computes
    it: each channel walks its own 16.16 step from its top-row value."""
    r = (GRAD_TR + ((y * GRAD_STEP_R) >> 16)) & 0x1F
    g = (GRAD_TG + ((y * GRAD_STEP_G) >> 16)) & 0x3F
    b = (GRAD_TB + ((y * GRAD_STEP_B) >> 16)) & 0x1F
    return (r << 11) | (g << 5) | b


if __name__ == '__main__':
    main()
