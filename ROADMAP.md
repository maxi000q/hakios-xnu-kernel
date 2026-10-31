# HakiOS — iOS 6 app edition: the 10-stage roadmap

This fork turns the upstream project on its head. Upstream boots the iOS 16.5 user space
natively on one specific phone (POCO X5 Pro, SM7325) through `fastboot boot` — everything
lives in RAM for the length of a session, and the phone must have an unlocked bootloader.

This fork's goal is the opposite shape, on purpose:

- **iOS 6, not iOS 16.5.** The system the app installs and boots is the 2012-2013 era one:
  XNU 2107, armv7, launchd/SpringBoard of 6.x. A 32-bit, PAC-less, PPL-less kernel is a
  tractable emulation target; an ARM64e XNU 10063 is not (see `docs/ios6-retarget.md`).
- **An app, not a boot.** No fastboot, no unlocked bootloader, no flashing, no root. A normal
  installable Android app that emulates the iPhone hardware in user space.
- **Storage, not RAM.** Upstream holds a whole session in memory. An app cannot: Android
  gives a normal process a few hundred MB of heap and the OS kills it otherwise. The guest's
  physical memory, its volumes and its VRAM are files on `/data` under `mmap` — the kernel
  can evict cold pages and refault them from flash, so the guest is bounded by free storage,
  not by RAM.
- **Mostly C++.** The interpreter, memory system, image pipeline and renderer are C++ built
  with the NDK; the Java layer is one thin Activity + the JNI bridge.
- **Adreno 619 first.** The reference devices are mid-range Qualcomm phones (Snapdragon
  690 / 720G / 750G / 480, all Adreno 619) — see `devices/adreno-619/`.

## The stages

Each stage is a self-contained piece that lands on the `ios6-emulator` branch with its own
commit; each builds on the previous one, and nothing is left stubbed where it can be real.

| Stage | Lands | What it is | Done when |
|---|---|---|---|
| 1 | `docs/`, `ROADMAP.md` | Foundation: fork, analysis of upstream, this roadmap, the Adreno 619 target doc, Android build hygiene in `.gitignore` | pushed to the fork |
| 2 | `native/memory/` | Storage-backed guest physical memory: a sparse image on app storage, `mmap(MAP_SHARED)` as the guest's RAM, bounds-checked accessors, page advice (`madvise`) and residency stats (`mincore`) | compiles on desktop g++, unit-tested |
| 3 | `native/cpu/` | ARMv7-A interpreter: the "CPU" of the emulated iPhone. Fetch/decode/execute of the ARM instruction set, conditions, load/store, branches — enough of ARMv7 to enter and run a 32-bit XNU | executes keystone-assembled test programs correctly; ~144 MIPS on a desktop host |
| 4 | `native/image/`, `sysimage/` retarget | iOS 6 system image pipeline: fetch the 6.1.6 IPSW (armv7 device) from Apple's servers like upstream's `fetch.sh` does for 16.5, unpack HFSX (not APFS), load the IMG3/8900 kernelcache, understand the armv7 dyld shared cache | kernel image parses; segments land in guest memory |
| 5 | `native/gpu/` | Adreno 619 renderer: GLES surface out of storage-backed VRAM, tile-renderer-friendly paths, a 30 fps cap for thermals | texture path verified on desktop (GL unavailable here) and wired to JNI |
| 6 | `native/core/`, JNI | The emulator engine: lifecycle, the run thread with a step budget per frame, halt/panic reporting, the JNI bridge that the app drives | app can start/stop/poll the guest from Java |
| 7 | `app/` | The Android app: first open shows the "Installing iOS 6" screen with a real progress bar (image create → download → unpack → kernel load → first boot), then the emulator screen | installs and runs on a device/emulator |
| 8 | `assets/`, tests | Bundled self-test kernel + the desktop test rig that proves stages 2-6 together without a phone | `tests/` green in CI and locally |
| 9 | `README.md`, `ci/` | README rewritten for the app, the 16.5→6 sweep over every remaining reference, CI workflow building the APK on every push | no "16.5" left where it means the target; CI builds |
| 10 | release | Build the release APK, tag, GitHub Release on the fork | a downloadable APK in Releases |

## Honesty clause

Stages 2-6 build real machinery that really executes armv7 code and really installs/unpacks
a real iOS 6 system image downloaded from Apple's own servers, the same way upstream
installs the 16.5 one. But one release does not produce a phone-in-an-app that runs
SpringBoard at full speed: the interpreter is a research core, first boots will be minutes
of logo, and full driver emulation (touch, storage, display of the real iPhone hardware
XNU 2107 expects) is the work of the stages after this one ships. The app will say what it
is doing at every step of the install screen, and will not pretend to be more than it is.

The 10 stages above are the deliverable of this effort; stage 10 builds and releases them.

## After the first release: what the machine names next

The ten stages shipped as release 1.2. The work after it follows the same
rule the roadmap always had — the guest itself says what is missing, in the
language of the halt report, and nothing is faked in front of it.

**1.3 (this tree): the VFP, the banked register file, and the exception
return.** The kernel's first halt after release 1.2 was the VFP probe of
`vfp_init`; the interpreter now carries a VFPv3-D16 (every encoding measured
off the keystone assembler — the s-register index puts its extra bit LOW,
the d-register HIGH; the unary space makes bits 7 and 22 opcode; the
fixed-point VCVT is a read-modify-write of Sd that does not encode a source
register at all), the Cortex-A8's identity values (FPSID 0x410330C0, MVFR0
0x11110222, MVFR1 0x00011111), LDRD/STRD, the banked register file with
per-mode r13/r14, the FIQ bank of r8-r12, per-mode SPSRs, MOVS pc / LDM
{pc}^ exception returns, user-bank LDM^/STM^, and a CPS whose mode form
switches banks (and whose interrupt forms, previously mis-decoded as data
processing, now really set and clear the masks). With that, Apple's own
6.1.3 kernel cache runs past `vfp_init`, through machine init and into the
prelinked kexts — over five million instructions — and stops at the first
Thumb-2 call. `tests/test_vfp.cpp` pins all of it; the whole rig stays
green.

**Named next, by the halt itself: the Thumb-2 interpreter.** The kexts are
compiled Thumb-2, and so is all of userland — iOS 6 apps will need it
everywhere. After that, the same run names the devices XNU 2107 probes and
the boot args iBoot would hand over. None of it is small, and none of it
will be pretended.
