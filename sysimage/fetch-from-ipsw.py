#!/usr/bin/env python3
"""Fetch one member of an iOS firmware archive without downloading the rest.

usage: fetch-from-ipsw.py [--url URL] [--size N] [--list [PATTERN]] MEMBER OUTPUT
       fetch-from-ipsw.py --list [PATTERN]

The IPSW is a ZIP archive on Apple's CDN; its directory is read with HTTP range
requests and the member is streamed (and inflated, if it is compressed) straight
to OUTPUT. A download that breaks off is taken up again: a member stored without
compression (the big disk images are) continues where it stopped, also when
OUTPUT is the partial file of an earlier run; a compressed one starts over.

Both the 16.5 archive of the native path (ZIP64) and the classic 32-bit ZIP of
the iOS 6 era are read; --url/--size override the defaults (the iOS 6 fetcher
under sysimage/ios6/ passes them), and --list prints the directory instead of
downloading, so the members a new IPSW version actually carries can be checked
before anything is pinned in a script.
"""
import argparse
import http.client
import os
import struct
import sys
import time
import urllib.request
import zlib

ATTEMPTS = 20

# the native path's image (iOS 16.5, iPhone X); the app path passes its own
DEFAULT_URL = ('https://updates.cdn-apple.com/2023SpringFCS/fullrestores/032-85130/6AB0D10A-8AF9-4F6C-8CF5-A1B315CABE88/'
               'iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw')
DEFAULT_SIZE = 6165698198


class Ipsw:
    def __init__(self, url, size):
        self.url = url
        self.size = size

    def fetch(self, start, end):
        request = urllib.request.Request(self.url, headers={'Range': f'bytes={start}-{end}'})
        return urllib.request.urlopen(request, timeout=120)

    def directory(self):
        """Every member: (name, method, csize, usize, data_offset), from the
        central directory. A ZIP64 archive still carries a classic EOCD with
        0xFFFFFFFF placeholders -- those mean "look at the ZIP64 record",
        not "empty archive at the end of the file"."""
        tail = self.fetch(self.size - 65536, self.size - 1).read()
        at = tail.rfind(b'PK\x05\x06')
        if at >= 0:
            nentries, = struct.unpack('<H', tail[at + 10:at + 12])
            cd_size, cd_offset = struct.unpack('<II', tail[at + 12:at + 20])
            if cd_offset != 0xFFFFFFFF and cd_size != 0xFFFFFFFF and nentries != 0xFFFF:
                return self._entries(cd_offset, cd_size, nentries)
        at = tail.rfind(b'PK\x06\x06')
        if at < 0:
            sys.exit('fetch-from-ipsw: neither a classic nor a ZIP64 end-of-central-directory in the last 64 KiB')
        cd_size, cd_offset = struct.unpack('<QQ', tail[at + 40:at + 56])
        return self._entries(cd_offset, cd_size, None)

    def _entries(self, cd_offset, cd_size, nentries):
        cd = b''
        p = cd_offset
        while len(cd) < cd_size:
            end = min(p + (1 << 20) - 1, self.size - 1)   # 416 past EOF otherwise
            chunk = self.fetch(p, end).read()
            if not chunk:
                break
            cd += chunk
            p += len(chunk)
        if len(cd) < cd_size:
            sys.exit(f'fetch-from-ipsw: the central directory is truncated ({len(cd)} of {cd_size} bytes)')
        entries = []
        p = 0
        while cd[p:p + 4] == b'PK\x01\x02':
            method, = struct.unpack('<H', cd[p + 10:p + 12])
            csize, usize, nl, el, cl = struct.unpack('<IIHHH', cd[p + 20:p + 34])
            offset, = struct.unpack('<I', cd[p + 42:p + 46])
            name = cd[p + 46:p + 46 + nl].decode()
            extra = cd[p + 46 + nl:p + 46 + nl + el]
            q = 0
            while q + 4 <= len(extra):
                tag, length = struct.unpack('<HH', extra[q:q + 4])
                if tag == 1:
                    values = list(struct.unpack('<' + 'Q' * (length // 8), extra[q + 4:q + 4 + length // 8 * 8]))
                    if usize == 0xFFFFFFFF:
                        usize = values.pop(0)
                    if csize == 0xFFFFFFFF:
                        csize = values.pop(0)
                    if offset == 0xFFFFFFFF:
                        offset = values.pop(0)
                q += 4 + length
            entries.append((name, method, csize, usize, offset))
            p += 46 + nl + el + cl
        if nentries is not None and len(entries) != nentries:
            sys.exit(f'fetch-from-ipsw: the directory lists {len(entries)} members, the EOCD says {nentries}')
        return entries

    def download(self, member, output):
        """One member out; returns the size written."""
        for name, method, csize, usize, offset in self.directory():
            if name != member:
                continue
            if method not in (0, 8):
                sys.exit(f'{member}: compression method {method} is not supported')
            header = self.fetch(offset, offset + 29).read()
            nl, el = struct.unpack('<HH', header[26:30])
            data = offset + 30 + nl + el
            written = -1
            for attempt in range(1, ATTEMPTS + 1):
                try:
                    written = self._member(data, method, csize, usize, output)
                except (OSError, http.client.HTTPException, zlib.error) as error:
                    print(f'{member}: {error}', file=sys.stderr)
                if written == usize:
                    break
                print(f'{member}: the download broke off, trying again ({attempt} of {ATTEMPTS})', file=sys.stderr)
                time.sleep(5)
            print(f'{member}: {written} bytes (expected {usize})')
            return 0 if written == usize else 1
        sys.exit(f'{member}: not in the archive')

    def _member(self, data, method, csize, usize, output):
        """One attempt at the member whose bytes start at DATA in the archive."""
        if method == 0:
            have = os.path.getsize(output) if os.path.exists(output) else 0
            if have > csize:
                have = 0
            if have < csize:
                end = min(data + csize - 1, self.size - 1)
                with self.fetch(data + have, end) as stream, open(output, 'ab' if have else 'wb') as out:
                    while chunk := stream.read(1 << 20):
                        out.write(chunk)
            return os.path.getsize(output)
        inflater = zlib.decompressobj(-15)
        written = 0
        end = min(data + csize - 1, self.size - 1)
        with self.fetch(data, end) as stream, open(output, 'wb') as out:
            while chunk := stream.read(1 << 20):
                chunk = inflater.decompress(chunk)
                out.write(chunk)
                written += len(chunk)
            tail = inflater.flush()
            out.write(tail)
        return written + len(tail)


def main():
    ap = argparse.ArgumentParser(add_help=True, description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--url', default=DEFAULT_URL, help='the IPSW on Apple\'s CDN')
    ap.add_argument('--size', type=int, default=DEFAULT_SIZE, help='its exact byte size')
    ap.add_argument('--list', nargs='?', const='', metavar='PATTERN',
                    help='print the member list (optionally filtered) and exit')
    ap.add_argument('member', nargs='?', help='the member to fetch')
    ap.add_argument('output', nargs='?', help='where to write it')
    args = ap.parse_args()

    ipsw = Ipsw(args.url, args.size)
    entries = ipsw.directory()
    if args.list is not None:
        for name, method, csize, usize, _ in entries:
            if args.list and args.list not in name:
                continue
            print(f'{name:52s} method={method} csize={csize:>10} usize={usize:>10}')
        return
    if not args.member or not args.output:
        ap.error('either --list or MEMBER OUTPUT')
    sys.exit(ipsw.download(args.member, args.output))


if __name__ == '__main__':
    main()
