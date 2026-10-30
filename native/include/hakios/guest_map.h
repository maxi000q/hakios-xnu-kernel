#pragma once

// The emulated machine, agreed on by every stage from here on: one iPhone
// 4-class device ("iPhone3,1", n90ap) as iOS 6.1.6 sees it. RAM and display
// live in one storage-backed physical image (storage_memory.h); the
// interpreter (stage 3), the image pipeline (stage 4) and the renderer
// (stage 5) all use this map.

#include <cstddef>
#include <cstdint>

namespace hakios::map {

// -- Guest physical layout --------------------------------------------------
//
//   0x00000000  +---------------------+
//               |  512 MiB   RAM      |  what the kernel sees
//   0x20000000  +---------------------+
//               |  16 MiB    VRAM     |  the emulated display (RGB565)
//   0x21000000  +---------------------+
//
// The whole range is one sparse file in the app's private storage, mapped
// MAP_SHARED (see storage_memory.h): the guest is bounded by free space on
// /data, not by the process heap, and it survives a process kill. On an
// Adreno 619 phone (64-256 GB of flash, 4-8 GB of RAM) that is the only way
// a 512 MiB "device" fits inside a normal app at all (docs/ios6-retarget.md).

constexpr uint64_t kRamBase  = 0x00000000;
constexpr size_t   kRamSize  = size_t(512) << 20;  // iPhone 4: 512 MiB
constexpr uint64_t kVramBase = 0x20000000;        // = kRamBase + kRamSize
constexpr size_t   kVramSize = size_t(16) << 20;
constexpr size_t   kPhysSize = kRamSize + kVramSize;

static_assert(kVramBase == kRamBase + kRamSize, "VRAM begins where RAM ends");
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
