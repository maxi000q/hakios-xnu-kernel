#include "hakios/dyld_cache.h"

namespace hakios {

bool DyldSharedCache::looks_like(const uint8_t* bytes, size_t len) {
    if (len < kMagicLen) return false;
    // The magic of every era begins "dyld_v1"; the platform follows, padded
    // with spaces ("dyld_v1 armv7", "dyld_v1  arm64", ...).
    return std::memcmp(bytes, "dyld_v1", 7) == 0;
}

bool DyldSharedCache::parse(const uint8_t* bytes, size_t len, std::string* err) {
    valid_ = false;
    platform_.clear();
    mappings_.clear();
    images_.clear();

    const auto fail = [err](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };
    if (len < 32) return fail("dyld cache: shorter than a header");
    if (!looks_like(bytes, len))
        return fail("dyld cache: magic is not 'dyld_v1 <platform>'");
    // "dyld_v1" then the platform (" armv7", "  arm64" -- the number of pad
    // spaces varies): skip the spaces, take the word, trim the tail
    size_t start = 7;
    while (start < kMagicLen && bytes[start] == ' ') start++;
    size_t end = start;
    while (end < kMagicLen && bytes[end] != ' ') end++;
    platform_.assign(reinterpret_cast<const char*>(bytes) + start, end - start);

    const uint32_t mapping_count = load_u32(bytes + 16);
    const uint32_t mapping_off   = load_u32(bytes + 20);
    const uint32_t images_count  = load_u32(bytes + 24);
    const uint32_t images_off    = load_u32(bytes + 28);

    if (mapping_count == 0 || mapping_count > 64)
        return fail("dyld cache: implausible mapping count " +
                    std::to_string(mapping_count));
    if (images_count > 16384)
        return fail("dyld cache: implausible image count " +
                    std::to_string(images_count));

    // mapping table: dyld_cache_mapping_info, 20 bytes each, 32-bit fields
    for (uint32_t i = 0; i < mapping_count; i++) {
        const size_t need = size_t(mapping_off) + size_t(i + 1) * 20;
        if (mapping_off >= len || need > len)
            return fail("dyld cache: mapping table runs off the end");
        const uint8_t* m = bytes + mapping_off + size_t(i) * 20;
        Mapping map;
        map.address     = load_u32(m);
        map.size        = load_u32(m + 4);
        map.file_offset = load_u32(m + 8);
        map.init_prot   = load_u32(m + 16);
        if (uint64_t(map.file_offset) + uint64_t(map.size) > len)
            return fail("dyld cache: mapping " + std::to_string(i) +
                        " reaches past the end of the file");
        mappings_.push_back(map);
    }

    // image table: dyld_cache_image_info, 16 bytes: address, mtime, inode,
    // pathFileOffset (an offset of a NUL-terminated path inside the file)
    for (uint32_t i = 0; i < images_count; i++) {
        const size_t need = size_t(images_off) + size_t(i + 1) * 16;
        if (images_off >= len || need > len)
            return fail("dyld cache: image table runs off the end");
        const uint8_t* e = bytes + images_off + size_t(i) * 16;
        Image img;
        img.address = load_u32(e);
        const uint32_t path_off = load_u32(e + 12);
        if (path_off >= len)
            return fail("dyld cache: image " + std::to_string(i) +
                        " path reaches past the end of the file");
        img.path.assign(reinterpret_cast<const char*>(bytes) + path_off,
                        strnlen(reinterpret_cast<const char*>(bytes) + path_off,
                                len - path_off));
        if (img.path.empty())
            return fail("dyld cache: image " + std::to_string(i) + " has no path");
        images_.push_back(std::move(img));
    }

    valid_ = true;
    return true;
}

std::string DyldSharedCache::describe() const {
    if (!valid_) return "dyld cache: not parsed";
    return "dyld cache " + platform_ + ": " + std::to_string(mappings_.size()) +
           " mappings, " + std::to_string(images_.size()) + " images";
}

}  // namespace hakios
