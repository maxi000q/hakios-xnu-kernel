#pragma once

// Storage-backed guest physical memory -- stage 2 of the app edition roadmap.
//
// The guest's "RAM" is not RAM: it is a file in the app's private storage,
// mapped MAP_SHARED, so Android can evict cold pages and refault them from
// flash. A guest is therefore bounded by free storage, not by the process
// heap limit, and it survives process death -- a killed app re-opens the
// same image, which is what makes "installed stays installed" possible.
//
// All offsets are guest physical addresses within [0, size()). The host and
// the ARMv7 guest are both little-endian (platform.h enforces that).

#include <cstddef>
#include <cstdint>
#include <string>

namespace hakios {

class StorageMemory {
public:
    struct Stats {
        size_t size_bytes;       // logical size of the mapping
        size_t page_bytes;       // host page size
        size_t pages;            // size_bytes / page_bytes
        size_t resident_pages;   // pages currently resident (mincore)
        size_t resident_bytes;
    };

    StorageMemory() = default;
    ~StorageMemory();
    StorageMemory(const StorageMemory&) = delete;
    StorageMemory& operator=(const StorageMemory&) = delete;
    StorageMemory(StorageMemory&& other) noexcept;
    StorageMemory& operator=(StorageMemory&& other) noexcept;

    // Create (or truncate) the backing file and map it. size must be a
    // positive multiple of the host page size. The mapping is advised
    // MADV_RANDOM: guest access is random, and readahead only burns flash
    // bandwidth that a mid-range phone does not have to spare.
    bool create(const std::string& backing_path, size_t size_bytes);

    // Map an existing image. expect_size == 0 accepts any page-multiple size.
    bool open_existing(const std::string& backing_path, size_t expect_size = 0);

    // Unmap and close. Safe to call any time; also run by the destructor.
    void close() noexcept;

    bool   valid() const { return base_ != nullptr; }
    size_t size() const { return size_; }
    const std::string& path() const { return path_; }

    // Direct pointer for hot paths (the stage-3 interpreter's fetch/decode
    // loop). Guest-physical address g lives at base() + g. Null if invalid.
    uint8_t* base() const { return base_; }

    // Bounds-checked typed accessors (little-endian guest integers).
    // Out-of-range reads return 0 and out-of-range writes are dropped;
    // either way last_error() says what happened.
    uint8_t  read8(uint64_t off);
    uint16_t read16(uint64_t off);
    uint32_t read32(uint64_t off);
    void     write8(uint64_t off, uint8_t v);
    void     write16(uint64_t off, uint16_t v);
    void     write32(uint64_t off, uint32_t v);

    // Bulk copies. A copy whose range leaves [0, size()) fails, sets
    // last_error(), and moves nothing.
    bool copy_in(uint64_t off, const void* src, size_t n);    // host -> guest
    bool copy_out(uint64_t off, void* dst, size_t n) const;   // guest -> host

    // -- Page management (advisory; safe from any thread) ------------------
    // A range about to run hot: the kernel should keep/fetch it resident.
    bool advise_willneed(uint64_t off, size_t len);
    // A range gone cold: its pages may be dropped now. For a MAP_SHARED file
    // mapping nothing is lost -- clean pages refault from flash, dirty pages
    // are written back first. The unit test proves the data survives.
    bool advise_dontneed(uint64_t off, size_t len);
    // Whole-mapping "random access, no readahead" (the default at create).
    bool advise_random();

    // Residency, via mincore: how much of the guest is actually in RAM right
    // now (the install screen shows this; "storage, not RAM" made visible).
    Stats  stats() const;
    size_t resident_pages(uint64_t off, size_t len) const;

    const std::string& last_error() const { return last_error_; }

private:
    bool map_fd(int fd, size_t size_bytes, const std::string& path);
    bool check_range(uint64_t off, size_t n) const;
    void fail(const std::string& msg) const;

    int          fd_   = -1;
    uint8_t*     base_ = nullptr;
    size_t       size_ = 0;
    std::string  path_;
    mutable std::string last_error_;
};

}  // namespace hakios
