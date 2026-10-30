# Legal

HakiOS is an independent open-source research project that ports the open-source XNU kernel to
ARM64 Qualcomm hardware. It is **not affiliated with, endorsed by, or connected to Apple Inc.**
"iOS", "iPhone", "XNU", "Apple" and other marks belong to their owners and are used here only to
describe what the code works with.

This file explains, in plain terms, what is and is not distributed by this repository. It is a
description of the project's intent and current state, **not legal advice**, and it does **not**
claim that any use of the project is lawful in your jurisdiction. Running the full system image
requires software and firmware you must obtain and are licensed to use yourself; whether you may
do so is your responsibility. If in doubt, consult a lawyer.

## What this repository contains

- **The kernel changes**: a patch to Apple's open-source XNU (`xnu/hakios.patch`) and added
  source files (`xnu/files/`). These are a derivative work of XNU and are licensed under the
  Apple Public Source License 2.0 (`LICENSE.APSL`). See `COMPLIANCE.md` for how the APSL terms
  are met.
- **Our own original code**: the boot loader (`xnuloader/`), user-space programs
  (`userland/src/`), the tools (`tools/`), the build and run scripts, the documentation, and the
  device layer under `devices/`. These are licensed under the MIT License (`LICENSE`).
- **Third-party open-source components** that are redistributed or fetched at build time, each
  under its own license. They are listed with origin and license in `THIRD_PARTY_LICENSES.md`.
- **Pre-built artifacts** in `prebuilt/`: the kernel, user-space programs and libraries built
  from this repository's own source. They carry the same licenses as the source they are built
  from.

## What this repository does NOT contain and never distributes

- **No part of iOS**: no iOS binary, image, cache, font, picture or screenshot. The
  two paths each build their system material on your machine:
  - the native path's user-space image is built from the iOS 16.5 IPSW, which
    `sysimage/fetch.sh` downloads from Apple's public servers under Apple's
    license, to your machine only;
  - the app edition's install fetches the kernel cache (7.4 MB) out of the
    iOS 6.1.3 IPSW from Apple's servers the same way (`sysimage/ios6/`), and
    stores the encrypted root filesystem DMG as the guest's system volume
    without decrypting it -- this release's emulator has no HFSX consumer,
    and the fetch tool reports what the volume is instead of pretending.
  Nothing of any of it is committed here.
- **No device firmware**: the GPU, Wi-Fi, audio DSP, modem, haptics and camera firmware is the
  vendor's (Qualcomm/Xiaomi). It is pulled from your own phone (`make firmware`) and is git-ignored.
- **No keys, certificates, activation data, or anything to impersonate a genuine Apple device to
  Apple's services.** Signing in to an Apple account is not possible here and is not a goal. The
  iOS 6 kernel cache is AES-256 encrypted as Apple ships it; the app edition's loader decrypts
  it only with a key and IV the user supplies from their own research, and the repository ships
  none, derives none, and circumvents nothing. The bundled self-test kernel needs no key because
  it is this project's own code, not Apple's.

## The Apple-free configuration

The core kernel, the boot loader and the open tools build and run with **nothing of Apple's**:

    make qemu-open

builds the kernel and a placeholder loader (no IPSW needed, see `xnuloader/mkidentity.sh
--placeholder`) and boots a small raw-syscall user space (`userland/src/hello.c`) under QEMU.
This is the configuration a contributor can build and test without downloading any closed Apple
software. The full iOS user space is an optional target that depends on software you supply.

## Reporting

If you believe a file in this repository infringes anyone's rights, open an issue (or email the
maintainer listed in the repository) and it will be reviewed and, where warranted, removed.
