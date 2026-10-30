#pragma once

// The emulated machine, agreed on by every stage from here on: one iPhone
// 4-class device ("iPhone3,1", n90ap) as iOS 6 sees it. RAM and display
// live in one storage-backed physical image (storage_memory.h); the
// interpreter (stage 3), the image pipeline (stage 4) and the renderer
// (stage 5) all use this map.
//
// -- Guest physical layout --------------------------------------------------
//
//   0x00000000  +---------------------+
//               |   (unmapped hole)   |  no device lives here
//   0x20000000  +---------------------+
//               |  16 MiB    VRAM     |  the emulated display (RGB565)
//   0x21000000  +---------------------+
//               |   (unmapped hole)   |
//   0x80000000  +---------------------+
//               |  512 MiB   RAM      |  where the A4 has it: an unmodified
//   0xA0000000  +---------------------+  iOS 6 kernel is linked to run here
//
// Stage 2 drew RAM at 0; stage 4 corrected it, and the reason is the kernel
// itself: XNU 2107 kernel caches are linked with __TEXT at 0x80001000 and
// early boot maps RAM 1:1 (virtual == physical), because every Apple ARM
// SoC of the era places SDRAM at 0x80000000. A loader that put segments
// where the old map had RAM would place the kernel where it can never run.
// The tests moved with it (tests/test_image.cpp pins both a segment that
// fits and one that must be refused).
//
// The whole 2.5 GiB of address space is one sparse file in the app's private
// storage, mapped MAP_SHARED (see storage_memory.h): unwritten holes cost
// nothing on flash, the guest is bounded by free space on /data, not by RAM,
// and it survives a process kill. A mapping this large needs a 64-bit host
// process -- every real target phone (all Adreno 619 SoCs) is one, and the
// app says so plainly on a 32-bit device instead of failing mysteriously.

#include <cstddef>
#include <cstdint>

namespace hakios::map {

constexpr uint64_t kVramBase = 0x20000000;        // the emulated display's own region
constexpr size_t   kVramSize = size_t(16) << 20;
constexpr uint64_t kRamBase  = 0x80000000;        // the A4's SDRAM, VA == PA at boot
constexpr size_t   kRamSize  = size_t(512) << 20;  // iPhone 4: 512 MiB
constexpr uint64_t kRamEnd   = kRamBase + kRamSize;
constexpr size_t   kPhysSize = size_t(kRamEnd);   // 0xA0000000 = 2.5 GiB, sparse

static_assert(kVramBase + kVramSize <= kRamBase, "VRAM ends before RAM begins");
static_assert(kPhysSize % 4096 == 0, "physical image is page-multiple");

// -- The emulated display (stage 5 blits this onto the Adreno 619) -----------

constexpr uint32_t kScreenWidth  = 640;  // iPhone 4 native pixels
constexpr uint32_t kScreenHeight = 960;
constexpr size_t   kVramBytesUsed =
    size_t(kScreenWidth) * kScreenHeight * 2;  // RGB565, 1.17 MiB

static_assert(kVramBytesUsed <= kVramSize, "the display fits in VRAM");

constexpr uint32_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t(r) >> 3) << 11 | (uint32_t(g) >> 2) << 5 | uint32_t(b) >> 3;
}

}  // namespace hakios::map
