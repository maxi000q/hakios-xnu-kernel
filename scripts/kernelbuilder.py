#!/usr/bin/env python3
"""kernelbuilder: the pieces both image generators share -- the Mach-O armv7
builder, the 'comp' LZSS wrapper and the IMG3 container -- so the fixtures of
tests/gen/image_fixtures.h and the bundled self-test kernel of
app/src/main/assets/demo-kernel.img3 are built by literally the same code the
loader parses.

Everything is built from scratch; only the formats match Apple's, verified
against the real kernelcache.release.n90 of the 6.1.3 IPSW (docs/ios6-retarget.md).
"""
import struct
import zlib

RING = 4096
MAXMATCH = 18


def fcc(s):
    """The fourcc convention of Apple's containers: a little-endian u32 read
    of the raw bytes, so the bytes spell backwards in a hex editor."""
    return (ord(s[0]) << 24) | (ord(s[1]) << 16) | (ord(s[2]) << 8) | ord(s[3])


class Lzss:
    """The exact inverse of native/image/kernel_lzss.cpp: a 4096-byte ring
    initialized to spaces, write pointer 4078, LSB-first flag bits, set bit =
    literal, clear bit = a two-byte match with a 12-bit absolute position and
    a length of 3 + (nibble)."""

    def __init__(self):
        self.ring = bytearray(b' ' * RING)
        self.pos = RING - MAXMATCH

    def put(self, byte):
        self.ring[self.pos] = byte
        self.pos = (self.pos + 1) % RING

    def copy_match(self, src_pos, count):
        out = bytearray()
        for _ in range(count):
            c = self.ring[src_pos]
            self.put(c)
            out.append(c)
            src_pos = (src_pos + 1) % RING
        return bytes(out)

    def compress(self, data):
        # The match search is the classic LZ77 one, on the output stream: a
        # match at distance d says data[i+j] == data[i-d+j], and when it is
        # longer than d the source range runs into the match's own output --
        # which is exactly what the decoder's byte-by-byte ring copy
        # reproduces. Searching the ring contents directly instead (as an
        # earlier version did) compares STALE bytes for those overlapping
        # matches, and the decoder then produces different bytes than the
        # search validated: the adler32 of the round trip no longer matches.
        # Distances never exceed the output so far, so the ring's
        # never-written spaces cannot be matched either.
        self.ring = bytearray(b' ' * RING)
        self.pos = RING - MAXMATCH
        out = bytearray()
        flag_pos = [None]
        bits = [0]

        def emit(bit):
            if bits[0] == 0:
                flag_pos[0] = len(out)
                out.append(0)
                bits[0] = 8
            if bit:
                out[flag_pos[0]] |= 1 << (8 - bits[0])
            bits[0] -= 1

        i = 0
        n = len(data)
        while i < n:
            best_len, best_d = 0, 0
            for d in range(1, min(RING, i) + 1):
                if data[i - d] != data[i]:
                    continue
                l = 0
                while (l < MAXMATCH and i + l < n
                       and data[i - d + l] == data[i + l]):
                    l += 1
                if l > best_len:
                    best_len, best_d = l, d
                    if l == MAXMATCH:
                        break
            if best_len >= 3:
                pos = (self.pos - best_d) % RING
                emit(0)
                out.append(pos & 0xFF)
                out.append(((pos >> 4) & 0xF0) | (best_len - 3))
                for k in range(best_len):
                    self.put(data[i + k])
                i += best_len
            else:
                emit(1)
                out.append(data[i])
                self.put(data[i])
                i += 1
        return bytes(out)


def lzss_compress(data):
    return Lzss().compress(data)


def macho_armv7_kernel(code, data_words=(), text_vmaddr=0x80001000,
                       entry_sp=0x08000000, bss_pages=8):
    """A small MH_KERNEL: __TEXT with the code, __DATA with u32 words, a
    __BSS of bss_pages * 4096, and one LC_UNIXTHREAD with pc = __TEXT start.
    Returns the image bytes."""
    text = code
    data = b''.join(struct.pack('<I', w) for w in data_words)
    ncmds = 4
    cmdsize = 56 * 3 + 16 + 4 * 17
    off_text = 28 + cmdsize
    off_data = off_text + len(text)
    bss_size = bss_pages * 4096

    def seg(name, addr, vmsize, fileoff, filesize):
        return struct.pack('<II16sIIIIIIII', 1, 56, name.encode(),
                           addr, vmsize, fileoff, filesize, 7, 5, 0, 0)

    words = [0] * 17
    words[13] = entry_sp
    words[15] = text_vmaddr
    words[16] = 0x13                       # SVC mode, flags clear
    body = struct.pack('<II', 1, 17) + struct.pack('<17I', *words)
    unixthread = struct.pack('<II', 5, len(body) + 8) + body

    cmds = (seg('__TEXT', text_vmaddr, max(0x1000, len(text)), off_text, len(text)) +
            seg('__DATA', text_vmaddr + 0x10000, 0x1000, off_data, len(data)) +
            seg('__BSS', text_vmaddr + 0x20000, bss_size, 0, 0) +
            unixthread)
    assert len(cmds) == cmdsize, (len(cmds), cmdsize)
    hdr = struct.pack('<7I', 0xFEEDFACE, 12, 9, 11, ncmds, cmdsize, 0)
    return hdr + cmds + text + data


def comp_wrap(macho):
    """The 'comp' prelinked-kernel wrapper: Apple's 336-byte header, the LZSS
    stream, padded to the AES block size so an encrypted sibling is possible."""
    stream = lzss_compress(macho)
    header = struct.pack('<6I10I16s256s',
                         fcc('comp'), fcc('lzss'),
                         zlib.adler32(macho) & 0xFFFFFFFF,
                         len(macho), len(stream), 1,
                         *([0] * 10), b'iPhone3,1', b'/S/L/F/kernel')
    assert len(header) == 336, len(header)
    blob = header + stream
    return blob + b'\0' * ((-len(blob)) % 16)


def img3_wrap(payload, ident='krnl', encrypted=False, chip=0x8930, board=0):
    def tag(name, body):
        total = 12 + len(body)
        total += (-total) % 4
        return (struct.pack('<III', fcc(name), total, len(body)) + body +
                b'\0' * (total - 12 - len(body)))

    tags = tag('TYPE', struct.pack('<I', fcc(ident)))
    tags += tag('DATA', payload)
    tags += tag('SEPO', struct.pack('<I', 2))
    tags += tag('CHIP', struct.pack('<I', chip))
    tags += tag('BORD', struct.pack('<I', board))
    if encrypted:
        for state in (1, 2):          # production + development, like the real file
            tags += tag('KBAG', struct.pack('<II', state, 0x100) + bytes(16) + bytes(32))
    return struct.pack('<5I', fcc('Img3'), 20 + len(tags), len(tags), 64,
                       fcc(ident)) + tags
