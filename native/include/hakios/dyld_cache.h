#pragma once

// The armv7 dyld shared cache reader -- stage 4 of the app edition roadmap:
// "understand the armv7 dyld shared cache".
//
// On iOS 6 the system's dynamic libraries live in one prelinked mapping per
// architecture, on the system volume at System/Library/Caches/com.apple.dyld/
// dyld_shared_cache_armv7 -- a few hundred MB where the 16.5 image keeps its
// arm64 cache split across cryptexes. The emulated system will map it the
// way dyld does (by segment), so stage 4 reads its header: the cache's own
// format is a 16-byte magic ("dyld_v1 armv7", space padded), the mapping
// table and the image (library) table, all 32-bit on this architecture.
//
// The reader is deliberately read-only and defensive: every count is capped,
// every offset bounds-checked, and nothing is mapped anywhere yet. What it
// produces is the truth about the cache -- which mappings it wants, at which
// addresses, and which libraries sit where -- which is what the later stages
// that grow a dyld will need, and what the install screen can honestly show
// ("dyld cache: 3 mappings, 1,204 libraries" instead of a spinner).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hakios/platform.h"

namespace hakios {

class DyldSharedCache {
public:
    static constexpr size_t kMagicLen = 16;    // "dyld_v1 armv7" + padding

    struct Mapping {
        uint32_t address = 0;
        uint32_t size = 0;
        uint32_t file_offset = 0;
        uint32_t init_prot = 0;
    };
    struct Image {
        uint32_t address = 0;
        std::string path;
    };

    // True when the bytes begin with "dyld_v1" (any platform string after).
    static bool looks_like(const uint8_t* bytes, size_t len);

    // Parse the header, the mapping table and the image table. Caps: 64
    // mappings, 16384 images -- both far above anything Apple shipped for
    // armv7, both far below anything that could exhaust memory here.
    bool parse(const uint8_t* bytes, size_t len, std::string* err);

    bool valid() const { return valid_; }
    const std::string& platform() const { return platform_; }   // "armv7"
    const std::vector<Mapping>& mappings() const { return mappings_; }
    const std::vector<Image>& images() const { return images_; }

    std::string describe() const;

private:
    bool valid_ = false;
    std::string platform_;
    std::vector<Mapping> mappings_;
    std::vector<Image> images_;
};

}  // namespace hakios
