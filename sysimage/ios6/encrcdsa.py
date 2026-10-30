#!/usr/bin/env python3
"""Report on an Apple disk image of the iOS 6 era: what it is, and what it
would take to read it.

usage: encrcdsa.py --describe FILE

Two states of the 6.1.3 root filesystem DMG exist:

  encrypted   the file starts with the magic "encrcdsa" and *everything*
              after it is ciphertext -- there is no koly trailer, no blkx
              table, no plist, nothing readable. Turning it into a volume
              takes the per-image AES key of that exact DMG (the "Root
              Filesystem Key" of the firmware keys that the research
              community publishes, used the way vfdecrypt uses it). This
              project ships no keys and does not decrypt: the emulator of
              this release stores the DMG as the guest's system volume and
              reports this header. When a later stage grows an HFSX driver,
              the pipeline grows the user-key decrypt step to match.

  decrypted   the file ends with the 512-byte "koly" trailer of a plain
              UDIF image: the XML plist and the blkx chunk tables are in
              the clear, the chunks inflate, and the volume header at byte
              1024 of the volume says which filesystem it is ('H+' HFS+,
              'HX' HFSX). That is the file a vfdecrypt'd DMG becomes, and
              what this tool can fully describe.

The layouts parsed here are the ones the format uses everywhere; the blkx
chunk table's shape (204-byte mish header, 40-byte entries, 64-bit offsets)
was verified against a real Apple UDIF, the root filesystem image of the
16.5 IPSW, read with HTTP range requests down to the byte.
"""
import argparse
import os
import plistlib
import struct
import sys
import zlib

KOLY = b'koly'
ENCR = b'encrcdsa'
MISH = b'mish'
HFS_SIGS = {b'H+': 'HFS+', b'HX': 'HFSX'}
CHUNK_ZERO = 0x00000000
CHUNK_RAW = 0x00000001
CHUNK_IGNORE = 0x00000002
CHUNK_COMMENT = 0x7FFFFFFE
CHUNK_ZLIB = 0x80000005
CHUNK_BZIP2 = 0x80000006
CHUNK_LZFSE = 0x80000007
CHUNK_END = 0xFFFFFFFF
CHUNK_NAMES = {CHUNK_ZERO: 'zero', CHUNK_RAW: 'raw', CHUNK_IGNORE: 'ignored',
               CHUNK_COMMENT: 'comment', CHUNK_ZLIB: 'zlib', CHUNK_BZIP2: 'bzip2',
               CHUNK_LZFSE: 'lzfse', CHUNK_END: 'end'}


def describe(path):
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        head = f.read(4096)
        f.seek(max(0, size - 512))
        tail = f.read(512)

    if head[:8] == ENCR:
        words = struct.unpack('>6I', head[8:32])
        print(f'{path}: Apple-encrypted disk image ("encrcdsa"), {size} bytes')
        print('  header: ' + ' '.join(f'w{i}=0x{w:08x}' for i, w in enumerate(words)))
        print(f'  everything after the header is ciphertext '
              f'(no koly trailer is present: {tail[:4] != KOLY})')
        print('  reading the volume needs this DMG\'s own AES key, which this project '
              'neither ships nor derives')
        return 0

    if tail[:4] != KOLY:
        print(f'{path}: neither an "encrcdsa" encrypted image nor a plain UDIF '
              '(no koly trailer); nothing to describe')
        return 1

    dfoff, dflen = struct.unpack('>2Q', tail[24:40])
    ploff, pllen = struct.unpack('>2Q', tail[216:232])
    print(f'{path}: UDIF disk image, {size} bytes (data fork {dflen} at {dfoff})')
    if not ploff or ploff + pllen > size:
        print('  the XML property list is missing or out of bounds')
        return 1
    with open(path, 'rb') as f:
        f.seek(ploff)
        pl = plistlib.loads(f.read(pllen))   # bounded: junk follows the plist
    blkx = pl.get('resource-fork', {}).get('blkx', [])
    print(f'  blkx partitions: {len(blkx)}')

    volume_blob = None
    with open(path, 'rb') as f:
        for b in blkx:
            d = b.get('Data', b'')
            if len(d) < 204 + 40 or d[:4] != MISH:
                continue
            sector_number, sector_count, data_offset = struct.unpack('>3Q', d[8:32])
            nchunks, = struct.unpack('>I', d[200:204])
            if nchunks > (1 << 20) or 204 + 40 * nchunks > len(d):
                print(f'    {str(b.get("Name", ""))[:28]:28s} chunk table is not sane')
                continue
            kinds = {}
            for i in range(nchunks):
                t, = struct.unpack('>I', d[204 + 40 * i:204 + 40 * i + 4])
                kinds[t] = kinds.get(t, 0) + 1
            named = ', '.join(f'{CHUNK_NAMES.get(k, hex(k))}x{v}'
                              for k, v in sorted(kinds.items()))
            print(f'    {str(b.get("Name", ""))[:28]:28s} sector {sector_number}, '
                  f'{sector_count} sectors, data at 0x{dfoff + data_offset:x}: {named}')
            # the first filesystem partition that carries zlib/raw chunks:
            # inflate its beginning and read the volume header
            if volume_blob is None and (CHUNK_ZLIB in kinds or CHUNK_RAW in kinds):
                base = dfoff + data_offset
                blob = b''
                for i in range(nchunks):
                    e = d[204 + 40 * i: 204 + 40 * (i + 1)]
                    t, _c, _s, _sc = struct.unpack('>4I', e[:16])
                    off, ln = struct.unpack('>2Q', e[16:32])
                    if t not in (CHUNK_ZLIB, CHUNK_RAW) or ln == 0:
                        continue
                    f.seek(base + off)
                    raw = f.read(min(ln, 1 << 20))
                    try:
                        blob += zlib.decompressobj(-15).decompress(raw) if t == CHUNK_ZLIB else raw
                    except zlib.error:
                        break
                    if len(blob) >= 1024 + 112:
                        break
                volume_blob = blob or None

    if volume_blob and len(volume_blob) >= 1026:
        sig = volume_blob[1024:1026]
        fs = HFS_SIGS.get(sig)
        if fs:
            print(f'  filesystem: {fs} (volume signature {sig!r} at volume byte 1024)')
            if len(volume_blob) >= 1024 + 52:
                blocksize, totalblocks, freeblocks = struct.unpack(
                    '>III', volume_blob[1024 + 40:1024 + 52])
                print(f'    block size {blocksize}, {totalblocks} blocks')
        else:
            print(f'  filesystem: not HFS+/HFSX (signature {sig!r})')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--describe', metavar='FILE')
    args = ap.parse_args()
    if not args.describe:
        ap.error('--describe FILE')
    sys.exit(describe(args.describe))


if __name__ == '__main__':
    main()
