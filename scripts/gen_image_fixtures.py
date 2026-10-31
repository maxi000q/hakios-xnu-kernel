#!/usr/bin/env python3
"""Generate tests/gen/image_fixtures.h: synthetic, self-made iOS-6-shaped kernel
images for the stage-4 loader tests.

Everything here is built from scratch -- no Apple bytes. What the fixtures
share with the real thing is only the *format*: the IMG3 tag walk, the
'comp' LZSS wrapper and the armv7 Mach-O layout, each verified against the
kernelcache.release.n90 of the real 6.1.3 IPSW (see docs/ios6-retarget.md).

The generator needs python3 (stdlib only) plus `openssl` for the AES-256-CBC
fixture. The generated header needs nothing: it is checked in, and the
desktop rig and CI compile it as-is. Regenerate after touching the formats:

    python3 scripts/gen_image_fixtures.py && tests/run_desktop_tests.sh
"""
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'tests', 'gen', 'image_fixtures.h')

# -- fixtures' fixed facts (mirrored by name in tests/test_image.cpp) --------
ENTRY_PC = 0x80001000
ENTRY_SP = 0x08000000
TEXT_VMADDR = 0x80001000
DATA_VMADDR = 0x80002000
BSS_VMADDR = 0x80003000
AES_KEY = bytes(range(0xA0, 0xC0))          # 32 bytes, test values only
AES_IV = bytes(range(0xC0, 0xD0))           # 16 bytes, test values only


def fcc(s):
    """The IMG3/Mach-O magic convention: a fourcc constant compared against a
    little-endian u32 read of the raw bytes (so the bytes spell backwards)."""
    return (ord(s[0]) << 24) | (ord(s[1]) << 16) | (ord(s[2]) << 8) | ord(s[3])


# -- LZSS: the exact inverse of native/image/kernel_lzss.cpp -----------------
RING = 4096
MAXMATCH = 18

class Lzss:
    def __init__(self):
        self.ring = bytearray([0x20]) * 0  # placeholder, set in reset()
        self.reset()

    def reset(self):
        self.ring = bytearray(b' ' * RING)
        self.pos = RING - MAXMATCH          # 4078, as the reference decoder

    def put(self, byte):
        self.ring[self.pos] = byte
        self.pos = (self.pos + 1) % RING

    def copy_match(self, src_pos, count):
        out = bytearray()
        for _ in range(count):
            c = self.ring[src_pos]
            self.put(c)                      # byte-by-byte: overlaps work
            out.append(c)
            src_pos = (src_pos + 1) % RING
        return bytes(out)

    def compress(self, data):
        self.reset()
        out = bytearray()
        flags = 0
        nbits = 0
        flag_pos = None

        def emit_flag_bit(bit):
            nonlocal flags, nbits, flag_pos
            if nbits == 0:
                flag_pos = len(out)
                out.append(0)
                flags = 0
                nbits = 8
            if bit:
                out[flag_pos] |= (1 << (8 - nbits))
            nbits -= 1

        i = 0
        while i < len(data):
            # longest match, 3..18 bytes, anywhere in the ring
            best_len, best_pos = 0, 0
            for pos in range(RING):
                l = 0
                p = pos
                while l < MAXMATCH and i + l < len(data) and self.ring[p] == data[i + l]:
                    l += 1
                    p = (p + 1) % RING
                if l > best_len:
                    best_len, best_pos = l, pos
                    if l == MAXMATCH:
                        break
            if best_len >= 3:
                emit_flag_bit(0)
                out.append(best_pos & 0xFF)
                out.append(((best_pos >> 4) & 0xF0) | (best_len - 3))
                self.copy_match(best_pos, best_len)
                i += best_len
            else:
                emit_flag_bit(1)
                out.append(data[i])
                self.put(data[i])
                i += 1
        return bytes(out)


def lzss_compress(data):
    return Lzss().compress(data)


# -- Mach-O armv7 --------------------------------------------------------------
def build_macho():
    text = bytes(((i * 7 + 3) & 0xFF) for i in range(96))      # "code": a pattern
    data = struct.pack('<8I', 0xDEADBEEF, 0xCAFEBABE, 1, 2, 3, 4, 5, 6)
    text_size, data_size = len(text), len(data)
    ncmds = 4
    cmdsize = 56 * 3 + 16 + 4 * 17        # three segments + LC_UNIXTHREAD
    header_end = 28 + cmdsize
    off_text = header_end
    off_data = off_text + text_size

    seg_cmd = lambda name, addr, vmsize, fileoff, filesize: struct.pack(
        '<II16sIIIIIIII', 1, 56, name.encode(),
        addr, vmsize, fileoff, filesize, 7, 5, 0, 0)

    def unixthread(pc, sp):
        words = [0] * 17
        words[13] = sp
        words[15] = pc
        words[16] = 0x13          # SVC mode, like the stage-3 CPU's reset
        body = struct.pack('<II', 1, 17) + struct.pack('<17I', *words)
        return struct.pack('<II', 5, len(body) + 8) + body

    cmds = b''
    cmds += seg_cmd('__TEXT', TEXT_VMADDR, 0x1000, off_text, text_size)
    cmds += seg_cmd('__DATA', DATA_VMADDR, 0x1000, off_data, data_size)
    cmds += seg_cmd('__BSS', BSS_VMADDR, 0x2000, 0, 0)
    cmds += unixthread(ENTRY_PC, ENTRY_SP)
    assert len(cmds) == cmdsize, (len(cmds), cmdsize)

    hdr = struct.pack('<7I', 0xFEEDFACE, 12, 9, 11, ncmds, cmdsize, 0)
    return hdr + cmds + text + data, text, data


# -- 'comp' prelinked-kernel wrapper -------------------------------------------
def build_comp(macho):
    stream = lzss_compress(macho)
    # the comp magics are stored FORWARD ('complzss' reads left to right),
    # unlike the IMG3 tag magics -- measured on the real 6.1.3 kernelcache
    le32 = lambda b: struct.unpack('<I', b)[0]
    header = struct.pack('<6I10I16s256s',
                         le32(b'comp'), le32(b'lzss'),
                         zlib.adler32(macho) & 0xFFFFFFFF,
                         len(macho), len(stream), 1,
                         *([0] * 10), b'iPhone3,1', b'/S/L/F/kernel')
    # Apple's own 6.1.3 kernelcache starts the stream at 0x180: the XNU
    # struct is 336 bytes, kextcache pads 48 more -- measured on the real file
    header += b'\0' * 48
    assert len(header) == 384, len(header)
    blob = header + stream
    # pad to the AES block size: encrypted DATA payloads are whole blocks
    pad = (-len(blob)) % 16
    return blob + b'\0' * pad


# -- IMG3 -----------------------------------------------------------------------
def tag(name, payload):
    data_len = len(payload)
    total = 12 + data_len
    total += (-total) % 4
    return struct.pack('<III', fcc(name), total, data_len) + payload + \
           b'\0' * (total - 12 - data_len)


def build_img3(payload, encrypted=False):
    tags = tag('TYPE', struct.pack('<I', fcc('krnl')))
    tags += tag('DATA', payload)
    tags += tag('SEPO', struct.pack('<I', 2))
    tags += tag('CHIP', struct.pack('<I', 0x8930))
    tags += tag('BORD', struct.pack('<I', 0))
    if encrypted:
        # two KBAGs like the real file: production and development. The IV/key
        # bytes are GID-wrapped on real hardware; here they are just markers,
        # never used -- the loader decrypts with the *unwrapped* test key.
        for state in (1, 2):
            kbag = struct.pack('<II', state, 0x100) + bytes(16) + bytes(32)
            tags += tag('KBAG', kbag)
    body = struct.pack('<5I', fcc('Img3'), 20 + len(tags), len(tags),
                       64, fcc('krnl')) + tags
    return body


def aes_cbc_encrypt(key, iv, plain):
    p = subprocess.run(
        ['openssl', 'enc', '-aes-256-cbc', '-nosalt', '-nopad',
         '-K', key.hex(), '-iv', iv.hex()],
        input=plain, capture_output=True, check=True)
    return p.stdout


def hex_array(name, blob):
    lines = []
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        lines.append('    ' + ' '.join(f'0x{b:02X},' for b in chunk))
    return f'static const uint8_t {name}[] = {{\n' + '\n'.join(lines) + '\n};\n'


def main():
    macho, text, data = build_macho()
    comp = build_comp(macho)

    # fat wrapper around the same thin image, to exercise the fat path:
    # big-endian fat header, one armv7 slice, 16 bytes of slack before it
    fat = struct.pack('>II', 0xCAFEBABE, 1)
    fat += struct.pack('>IIIII', 12, 9, 8 + 20 + 16, len(macho), 0)
    fat += b'\0' * 16 + macho

    plain_img3 = build_img3(comp, encrypted=False)
    cipher = aes_cbc_encrypt(AES_KEY, AES_IV, comp)
    enc_img3 = build_img3(cipher, encrypted=True)

    # a dyld shared cache fixture: magic + 2 mappings + 3 images + paths.
    # mapping sizes stay inside the fixture file: the parser (rightly) refuses
    # a mapping that claims to reach past the end of the file it lives in.
    paths = [b'/usr/lib/libSystem.B.dylib', b'/System/Library/CoreServices/SpringBoard.app/SpringBoard',
             b'/usr/lib/libobjc.A.dylib']
    paths_blob = b'\0'.join(paths) + b'\0'
    header_len = 32
    mapping_off = header_len
    mappings = b''
    # sizes must fit the fixture file: the parser refuses a mapping that
    # reaches past the end, exactly as it would refuse a corrupt real cache
    paths_start = mapping_off + 2 * 20 + len(paths) * 16
    mappings += struct.pack('<IIIII', 0x40000000, 0x40, paths_start, 5, 0)
    mappings += struct.pack('<IIIII', 0x48000000, 0x40, 0x20, 3, 0)
    images_off = mapping_off + len(mappings)
    images = b''
    for i, p in enumerate(paths):
        images += struct.pack('<IIII', 0x40000000 + 0x1000 * (i + 1), 0, 0,
                              images_off + len(paths) * 16 + sum(len(q) + 1 for q in paths[:i]))
    cache = (b'dyld_v1 armv7' + b' ' * 3 +
             struct.pack('<4I', 2, mapping_off, len(paths), images_off) +
             mappings + images + paths_blob)

    # the compressed stream inside kComp is whatever the compressor made of
    # the Mach-O; the tests recompute sizes from the fixtures themselves
    assert comp[:4] == b'comp'          # stored forward, like the real cache
    assert struct.unpack('<I', comp[12:16])[0] == len(macho)

    parts = []
    parts.append("""// Generated by scripts/gen_image_fixtures.py -- DO NOT EDIT.
//
// Synthetic iOS-6-shaped fixtures for the stage-4 loader tests (see the
// generator's docstring): a small armv7 Mach-O kernel, wrapped and re-wrapped
// the way Apple wraps its kernel caches, all built here from scratch. No
// Apple bytes are in this file; only the formats match the real thing.

#pragma once

#include <cstddef>
#include <cstdint>

namespace hakios::fixtures {
""")
    parts.append(hex_array('kMachO', macho))
    parts.append(hex_array('kMachOFat', fat))
    parts.append(hex_array('kComp', comp))
    parts.append(hex_array('kImg3Plain', plain_img3))
    parts.append(hex_array('kImg3Encrypted', enc_img3))
    parts.append(hex_array('kDyldCache', cache))
    parts.append(f"""
// the facts the tests expect
constexpr uint32_t kEntryPc   = 0x{ENTRY_PC:08X};
constexpr uint32_t kEntrySp   = 0x{ENTRY_SP:08X};
constexpr uint32_t kTextVaddr = 0x{TEXT_VMADDR:08X};
constexpr uint32_t kDataVaddr = 0x{DATA_VMADDR:08X};
constexpr uint32_t kBssVaddr  = 0x{BSS_VMADDR:08X};
constexpr size_t   kTextSize  = {len(text)};
constexpr size_t   kDataSize  = {len(data)};
constexpr size_t   kMachOSize = {len(macho)};
constexpr size_t   kCompSize  = {len(comp)};
static const uint8_t kAesKey[32] = {{ {', '.join(f'0x{b:02X}' for b in AES_KEY)} }};
static const uint8_t kAesIv[16]  = {{ {', '.join(f'0x{b:02X}' for b in AES_IV)} }};
constexpr uint32_t kDataWord0  = 0xDEADBEEF;
}}  // namespace hakios::fixtures
""")
    with open(OUT, 'w') as f:
        f.write('\n'.join(parts))
    print(f'wrote {os.path.relpath(OUT)}: macho={len(macho)} comp={len(comp)} '
          f'img3={len(plain_img3)} img3-enc={len(enc_img3)} cache={len(cache)}')


if __name__ == '__main__':
    main()
