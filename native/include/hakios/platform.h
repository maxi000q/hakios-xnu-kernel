#pragma once

// Small platform layer shared by the hakios native core (stage 2+ of the app
// edition roadmap). Target hosts: Android (NDK, arm64-v8a), and Linux/macOS
// for the desktop test rig (tests/run_desktop_tests.sh). Windows is not one.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <unistd.h>

namespace hakios {

// The guest is ARMv7 little-endian; every host we build on is little-endian
// too, so guest integers never need byte swapping -- only byte-wise copies
// that the compiler turns into plain loads and stores.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "hakios core requires a little-endian host");
#endif

inline size_t page_size() {
    static const size_t ps = []() -> size_t {
#ifdef _SC_PAGESIZE
        const long v = ::sysconf(_SC_PAGESIZE);
        return (v > 0) ? static_cast<size_t>(v) : size_t(4096);
#else
        return size_t(4096);
#endif
    }();
    return ps;
}

inline size_t round_up_to_page(size_t n) {
    const size_t ps = page_size();
    return (n + ps - 1) / ps * ps;
}

inline uint64_t round_down_to_page64(uint64_t n) {
    const uint64_t ps = static_cast<uint64_t>(page_size());
    return n & ~(ps - 1);
}

inline uint16_t load_u16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline uint32_t load_u32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline void store_u16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, sizeof v); }
inline void store_u32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof v); }

}  // namespace hakios
