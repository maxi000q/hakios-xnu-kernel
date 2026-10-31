# The iOS 6 system image: 6.1.3 (10B329), iPhone 4

What the app edition installs, where it comes from, and what each file is.
Everything here was verified against Apple's own archive, member by member,
with the range-request reader in `../fetch-from-ipsw.py` -- no file of iOS is
in this repository, ever; `fetch.sh` downloads from Apple's servers to your
machine only, exactly the contract the 16.5 fetcher of the native path has.

## Why 6.1.3 and not 6.1.6

The roadmap said "iOS 6.1.6 for an armv7 device (iPhone 4; exact build
verified in stage 4)" -- and the verification changed the number. Apple
shipped 6.1.6 (10B500) for the **iPhone 3GS (iPhone2,1)** and the **iPod
touch 4G (iPod4,1)** only, as security updates for the devices that cannot
run iOS 7. The **iPhone 4 GSM (iPhone3,1)** -- the machine this emulator is:
512 MiB of RAM, the 640x960 display of `native/include/hakios/guest_map.h`,
the A4 -- ends its iOS 6 line at **6.1.3, build 10B329**:

| | |
|---|---|
| IPSW | `iPhone3,1_6.1.3_10B329_Restore.ipsw` (938,891,219 bytes, SHA-1 `f8c53328656f2708e1d2bcb793f12ae46dd4eb23`) |
| URL | Apple's own CDN, pinned in `fetch.sh` |
| archive | a classic 32-bit ZIP (31 members) -- not ZIP64 like the 16.5 one, which is why `fetch-from-ipsw.py` learned both |
| kernel | `kernelcache.release.n90`, 7,416,132 bytes, deflated in the archive |
| root filesystem | `048-2748-005.dmg`, 901,928,448 bytes stored |

## What the two files are

**The kernel cache** is an IMG3 container (magic bytes `3gmI` at offset 0):
header, `TYPE krnl`, `DATA` (7,413,724 bytes), `SEPO` 2, two `KBAG`s
(production and development, AES-256), `SHSH`, `CERT`. The DATA payload is
AES-256-CBC encrypted -- the KBAGs hold the IV and key wrapped with the
device group key, and the *unwrapped* per-file IV+key are what the research
community publishes as firmware keys. The loader (`native/image/`) parses
the container and decrypts with a key the user supplies during install;
this project ships no keys and wraps or derives nothing. (Apple stores the
logical size in the DATA tag -- the ciphertext itself is whole 16-byte
blocks, so the last partial block is not in the file; the loader drops
that unreachable tail instead of guessing at it.)

After decryption the payload is a `'comp'`-headered LZSS stream (the
magics spell FORWARD, `complzss` in a hex editor -- unlike the IMG3 tag
magics of the same file, which spell backwards; the header is 384 bytes,
not the XNU struct's 336; and `uncompressedSize` holds the pre-prelink
kernel, 6,343,680, while the stream really yields 13,393,815 bytes). It
inflates to a **thin armv7 Mach-O** -- a *prelinked* kernel: `__TEXT`,
`__DATA`, `__KLD`, `__PRELINK_TEXT` (the kexts, 9.4 MB), `__PRELINK_INFO`
(the kext plists, whose XML is exactly where its fileoff points -- the
byte-perfect proof of the decode) -- linked to run at 0x80001000 with the
entry at 0x80086084, which is why the emulated iPhone's RAM lives at
0x80000000 (`native/include/hakios/guest_map.h`).

**The root filesystem DMG** is not a plain disk image like the 16.5 rootfs:
it starts with the magic `encrcdsa` and *everything* after that header is
ciphertext -- there is no `koly` trailer, no chunk table, nothing readable
(the 16.5 rootfs, by contrast, is a plain UDIF whose LZFSE chunk table this
project's tools can read; that difference is why "unpack HFSX" is not what
the stage-4 plan imagined it would be). Reading the volume needs this
DMG's own AES key -- the "Root Filesystem Key" of the 6.1.3 firmware keys,
used the way `vfdecrypt` uses it -- and this release does not do that:
the emulator has no HFSX driver yet, so the app installs the DMG as the
guest's system volume, reports what it is (`encrcdsa.py --describe`), and
nothing more. When a later stage grows the driver, the pipeline grows the
user-key decrypt step to match, and `encrcdsa.py` already knows how to
describe the decrypted result down to the HFSX volume signature.

## What fetch.sh does

```
KERNEL_ONLY=1 ./fetch.sh    # the kernel cache only (7.4 MB) -- CI's shape
./fetch.sh                  # both: + the ~900 MB rootfs DMG
```

It reads the archive directory first (a changed member name fails before
hundreds of megabytes move), fetches the members by HTTP range requests
without ever downloading the whole IPSW, and writes:

```
kernelcache.release.n90    the install pipeline's input
048-2748-005.dmg           the guest's system volume, stored
directory.txt              what the archive actually contained, for the record
```

## The verified run, on the record

The whole chain has been executed against Apple's own file, end to end,
with a key supplied at run time from published research (never stored in
this repository): fetch from Apple's servers, IMG3 parse, AES-256-CBC
decrypt, comp/LZSS inflate to the 13.4 MB prelinked armv7 Mach-O, segments
into the emulated iPhone's RAM at their linked addresses, and the CPU
turned loose at 0x80086084 -- Apple's own entry point. The kernel runs its
real boot path for millions of instructions (start, the exception vectors,
CP15 bring-up through the A4's system coprocessor, pmap and machine init)
and halts at the first thing the machine does not have: the VFP unit it
probes during `vfp_init`, reported by name with the PC and the instruction
word. The next stages of the machine are therefore named by the kernel
itself: VFP, the devices XNU 2107 probes, and the boot args iBoot would
hand over -- none of them pretended, all of them on the roadmap.

## The dyld shared cache

On the system volume, at `System/Library/Caches/com.apple.dyld/
dyld_shared_cache_armv7`, lives the armv7 dyld shared cache: one prelinked
mapping per architecture where the 16.5 image keeps its arm64 caches split
across cryptexes. It sits inside the encrypted volume, so this stage's
reader (`native/image/dyld_cache.cpp`) understands the format -- the
`dyld_v1 armv7` header, the mapping table, the image table -- and the
app's install report can state what a cache contains the day a later stage
maps one. Nothing pretends to read it out of ciphertext.
