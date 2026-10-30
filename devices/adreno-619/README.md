# Adreno 619: the first-class target

The app edition of HakiOS is built for, and tuned for, the mid-range Qualcomm family
whose GPU is the **Adreno 619**:

| SoC | Phones of the class |
|---|---|
| SM6350 (Snapdragon 690 5G) | e.g. OnePlus Nord N10 class |
| SM7125 (Snapdragon 720G) | e.g. Redmi Note 9 Pro / realme 6 Pro class |
| SM7225 (Snapdragon 750G) | e.g. Galaxy A52 5G / Mi 10i class |
| SM4350 (Snapdragon 480) | entry 5G class |

(That class is the point, not any one model: every phone in it is a normal retail device.)

## Why this family

- It is what the target user base actually holds: 4-8 GB RAM, 64-256 GB storage, a
  1080p panel and a tile-based GPU.
- An in-app emulator competes with the whole Android system for RAM — impossible to win,
  unnecessary to try. The storage-backed design (stage 2) sidesteps the fight: the guest
  is files on `/data`, and Adreno 619 phones ship with tens of GB of it.
- GLES 3.2 is enough for everything the emulator's display path needs (a texture upload
  and a quad), so no Vulkan requirement, no per-vendor extensions, nothing that breaks on
  the long tail of these devices.

## Tuning decisions made for the 619

1. **30 fps cap.** A 60 Hz texture re-upload of a guest framebuffer is the worst case for
   a tile-based GPU and spins the big core up. The core thread gets a fixed instruction
   budget per frame and the renderer a 33 ms pacing; frame rate is not the point —
   completing the install and booting the kernel is.
2. **RGB565 guest framebuffer.** Halves the bytes per page fault from guest VRAM; iOS 6
   ran 32-bit RGBA but the emulated display pipeline quantizes early, on upload, off the
   CPU path.
3. **One texture, one quad, no readback.** No glReadPixels, no client-side array copies,
   no feedback loops into guest memory.
4. **Cold pages are free.** The storage-backed VRAM means a paused app's display memory
   can be fully evicted by Android and refault on resume; nothing is held in the heap
   across a background kill.

## Out of scope here

Upstream's fastboot/native path needs a specific device port (`devices/poco-x5-pro/`) —
device tree, UEFI, firmware. The app path does not: an interpreted guest has no use for
the phone's real device tree, interrupts or firmware. This directory therefore holds
target-class facts and tuning only. If the native path is ever ported to an Adreno 619
device, it gets its own `devices/<phone>/` next to this one.
