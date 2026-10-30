# The iOS 6 retarget: what changes and why

Upstream targets iOS 16.5 (kernel `xnu-8796.121.2`, build 20F66, iPhone X system image)
booted natively on SM7325. This fork targets iOS 6 inside an app on Adreno 619 phones.
Every difference between the two follows from that one decision.

## Kernel

| | Upstream (16.5) | This fork (6.x) |
|---|---|---|
| XNU | xnu-8796.121.2 | xnu-2107 series (the iOS 6 line) |
| Verified build | 20F66, iPhone X | **6.1.3 (10B329), iPhone3,1** — pinned in stage 4 |
| ISA | ARM64, ARM64e kernel | ARMv7-A, 32-bit |
| Pointer auth | PAC | none |
| Page tables | ARM64 long descriptors | ARMv7 short descriptors |
| Sealed volume / cryptex | yes | no such thing in 2012 |
| Kernelcache wrapper | IMG4 / IM4P | IMG3 (`3gmI` at offset 0, ident `krnl`) |

The stage-4 verification, done against Apple's own archive: **iOS 6.1.6 was
never shipped for the iPhone 4** — it exists for the iPhone 3GS and the iPod
touch 4G only. The last iOS 6 of the emulated machine (iPhone3,1, the A4,
512 MiB) is **6.1.3, build 10B329**, and that is what the app installs
(`sysimage/ios6/README.md` has the member-by-member record).

The reason to go back: an in-app interpreter can afford to implement ARMv7. It is a fixed,
small, well-documented 32-bit instruction set; there is no PAC, no PPL, no TZ `trampoline`,
and the MMU model (short-descriptor, per-domain) is a few hundred lines instead of a
multi-level walk with permission indirections. Emulating 2012 hardware is a weekend
compared to emulating 2023 hardware, and iOS 6 is the era the target phones' GPUs feel at
home with anyway.

One consequence stage 4 made physical: the kernel cache is **linked at
0x80001000**, and XNU 2107 maps RAM 1:1 at boot, because every Apple ARM
SoC of the era places SDRAM at 0x80000000. The emulated iPhone's RAM moved
there (`native/include/hakios/guest_map.h`) — stage 2 had drawn it at
physical 0, which no unmodified iOS 6 kernel could run from. The storage
image now covers 2.5 GiB of address space; it stays sparse, so the cost on
flash is the pages actually written.

## System image

- Upstream `sysimage/fetch.sh` pulls, from Apple's public CDN, the 16.5 iPhone X IPSW
  members: an APFS root filesystem (4.7 GB), an OS cryptex with the arm64 dyld shared
  cache, an App cryptex.
- iOS 6.1.3 for the iPhone 4 (`iPhone3,1`; the build stage 4 verified, see
  above) has: one root filesystem DMG (~860 MB, `encrcdsa`-encrypted), the
  kernel cache inside the IPSW, an armv7 dyld shared cache on the system
  volume, and nothing else. No cryptexes, no sealed root, no update volumes.

The stage 4 pipeline keeps upstream's contract — "nothing of iOS is in the repository; the
user's own machine downloads it from Apple's servers" — with a smaller, simpler payload.
What it works with changes accordingly: IMG3 instead of IMG4, an encrypted rootfs
volume stored as the guest's system disk instead of an APFS image unpacked on the
desktop, armv7 cache headers instead of arm64. The kernel side is real end to end
(parse, optional decrypt with a user-supplied key, LZSS inflate, segments into guest
RAM); the filesystem side is honest about its boundary — the 6.x DMG is encrypted
whole, and this release's emulator has no HFSX consumer yet, so the volume is
installed and reported, not unpacked (`sysimage/ios6/README.md`).

## Memory: storage, not RAM

Upstream lives one session in RAM by design ("a reboot brings the phone back to Android").
An app cannot adopt that design: Android allots a normal process a few hundred MB of
anonymous memory, kills it when it exceeds the limit, and Adreno 619 phones have 4-8 GB
total which the app must share with the rest of the system.

So the app's guest memory is **not memory**: guest physical RAM, the system volume and
VRAM are files in the app's private storage, accessed through `mmap(PROT_READ|PROT_WRITE,
MAP_SHARED)`. The pages count against storage, not the heap limit; the kernel can evict
cold ones under pressure and refault them from flash; the guest is bounded by free space
on `/data`, which on these phones is tens of GB. Stage 2 implements this, including the
advice API (`madvise`) the interpreter uses to keep hot pages resident and the residency
counter (`mincore`) the UI shows. Upstream's "reboot and it is gone" becomes "the install
stays installed; a cold guest is a cold page".

## Boot: app, not fastboot

Upstream's chain is `fastboot boot` → BootShim → UEFI port → xnuloader → XNU, with the
phone's own firmware pulled out of its own partitions. That requires an unlocked
bootloader and control of the whole machine — by design, because native execution needs
it.

An in-app emulator needs none of that and can have none of it: the app owns a normal
Android process, its interpreter is the CPU, its storage image is the machine, its GLES
surface is the display. The cost is speed — a research interpreter runs a 2012 32-bit
kernel at "minutes to first logo" pace, not seconds — and the win is that every retail
Adreno 619 phone can install it unmodified. The bootloader, UEFI port, device tree and
firmware machinery of upstream stay in the tree for the native path; the app path
(`devices/adreno-619/`) does not use them.

## GPU: Adreno 619

The primary target family (Snapdragon 690 / 720G / 750G / 480, Adreno 619) supports
OpenGL ES 3.2. The renderer (stage 5) is deliberately modest: a storage-backed RGB565
framebuffer the guest writes through the interpreter, uploaded as a texture per frame,
no client-side copies, a 30 fps cap to keep the big core at low clocks — a tile-based
GPU's worst enemy is a 60 Hz upload of a texture it will re-read. See
`devices/adreno-619/README.md`.

## What upstream machinery survives untouched

The QEMU path, the POCO X5 Pro device port, the native boot chain: all of it stays, as
the native 16.5 branch of the same tree. This fork adds the iOS 6 in-app emulation path
on the side rather than removing anything — the two share the image pipeline, the docs
and (from stage 4) the IPSW fetch tooling.
