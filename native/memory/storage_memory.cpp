// Storage-backed guest physical memory -- implementation. Stage 2.
//
// Plain POSIX: open/ftruncate/mmap/madvise/mincore, identical on Android and
// on the desktop test hosts. No Android APIs, no JNI -- that comes in stage 6.

#include "hakios/storage_memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <utility>
#include <vector>

#include "hakios/platform.h"

namespace hakios {

StorageMemory::~StorageMemory() { close(); }

StorageMemory::StorageMemory(StorageMemory&& other) noexcept
    : fd_(other.fd_), base_(other.base_), size_(other.size_), path_(std::move(other.path_)) {
    other.fd_   = -1;
    other.base_ = nullptr;
    other.size_ = 0;
}

StorageMemory& StorageMemory::operator=(StorageMemory&& other) noexcept {
    if (this != &other) {
        close();
        fd_   = std::exchange(other.fd_, -1);
        base_ = std::exchange(other.base_, nullptr);
        size_ = std::exchange(other.size_, size_t(0));
        path_ = std::move(other.path_);
    }
    return *this;
}

void StorageMemory::close() noexcept {
    if (base_ != nullptr) {
        ::munmap(base_, size_);
        base_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    size_ = 0;
    path_.clear();
}

void StorageMemory::fail(const std::string& msg) const { last_error_ = msg; }

bool StorageMemory::map_fd(int fd, size_t size_bytes, const std::string& path) {
    void* base = ::mmap(nullptr, size_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        const int e = errno;
        ::close(fd);
        fail("mmap " + path + ": " + std::strerror(e));
        return false;
    }
    // Guest access is random by nature; readahead on flash would only waste
    // bandwidth and evict useful pages on a mid-range phone.
    ::madvise(base, size_bytes, MADV_RANDOM);

    fd_   = fd;
    base_ = static_cast<uint8_t*>(base);
    size_ = size_bytes;
    path_ = path;
    return true;
}

bool StorageMemory::create(const std::string& backing_path, size_t size_bytes) {
    close();
    const size_t ps = page_size();
    if (size_bytes == 0 || size_bytes % ps != 0) {
        fail("create: size must be a positive multiple of the page size");
        return false;
    }
    const int fd = ::open(backing_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        const int e = errno;
        fail("open " + backing_path + ": " + std::strerror(e));
        return false;
    }
    // Sparse: this reserves the logical size without writing anything; only
    // pages the guest actually touches ever cost flash.
    if (::ftruncate(fd, static_cast<off_t>(size_bytes)) != 0) {
        const int e = errno;
        ::close(fd);
        fail("ftruncate " + backing_path + ": " + std::strerror(e));
        return false;
    }
    return map_fd(fd, size_bytes, backing_path);
}

bool StorageMemory::open_existing(const std::string& backing_path, size_t expect_size) {
    close();
    const int fd = ::open(backing_path.c_str(), O_RDWR);
    if (fd < 0) {
        const int e = errno;
        fail("open " + backing_path + ": " + std::strerror(e));
        return false;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        ::close(fd);
        fail("fstat " + backing_path + ": " + std::strerror(e));
        return false;
    }
    const size_t actual = static_cast<size_t>(st.st_size);
    if (actual == 0 || actual % page_size() != 0) {
        ::close(fd);
        fail("open " + backing_path + ": not a guest image (bad size)");
        return false;
    }
    if (expect_size != 0 && actual != expect_size) {
        ::close(fd);
        fail("open " + backing_path + ": size mismatch");
        return false;
    }
    return map_fd(fd, actual, backing_path);
}

bool StorageMemory::check_range(uint64_t off, size_t n) const {
    if (base_ == nullptr) {
        fail("range check: no mapping");
        return false;
    }
    if (off > size_ || n > size_ - off) {
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "guest range out of bounds: off=0x%llx len=%zu size=%zu",
                      static_cast<unsigned long long>(off), n, size_);
        fail(buf);
        return false;
    }
    return true;
}

uint8_t StorageMemory::read8(uint64_t off) {
    if (!check_range(off, 1)) return 0;
    return base_[off];
}

uint16_t StorageMemory::read16(uint64_t off) {
    if (!check_range(off, 2)) return 0;
    return load_u16(base_ + off);
}

uint32_t StorageMemory::read32(uint64_t off) {
    if (!check_range(off, 4)) return 0;
    return load_u32(base_ + off);
}

void StorageMemory::write8(uint64_t off, uint8_t v) {
    if (!check_range(off, 1)) return;
    base_[off] = v;
}

void StorageMemory::write16(uint64_t off, uint16_t v) {
    if (!check_range(off, 2)) return;
    store_u16(base_ + off, v);
}

void StorageMemory::write32(uint64_t off, uint32_t v) {
    if (!check_range(off, 4)) return;
    store_u32(base_ + off, v);
}

bool StorageMemory::copy_in(uint64_t off, const void* src, size_t n) {
    if (!check_range(off, n)) return false;
    if (n > 0) std::memcpy(base_ + off, src, n);
    return true;
}

bool StorageMemory::copy_out(uint64_t off, void* dst, size_t n) const {
    if (!check_range(off, n)) return false;
    if (n > 0) std::memcpy(dst, base_ + off, n);
    return true;
}

bool StorageMemory::advise_willneed(uint64_t off, size_t len) {
    if (base_ == nullptr || off >= size_ || len == 0) {
        fail("advise_willneed: bad range");
        return false;
    }
    const uint64_t start = round_down_to_page64(off);
    const uint64_t end   = std::min<uint64_t>(round_up_to_page(off + len), size_);
    if (::madvise(base_ + start, static_cast<size_t>(end - start), MADV_WILLNEED) != 0) {
        const int e = errno;
        fail(std::string("madvise WILLNEED: ") + std::strerror(e));
        return false;
    }
    return true;
}

bool StorageMemory::advise_dontneed(uint64_t off, size_t len) {
    if (base_ == nullptr || off >= size_ || len == 0) {
        fail("advise_dontneed: bad range");
        return false;
    }
    const uint64_t start = round_down_to_page64(off);
    const uint64_t end   = std::min<uint64_t>(round_up_to_page(off + len), size_);
    if (::madvise(base_ + start, static_cast<size_t>(end - start), MADV_DONTNEED) != 0) {
        const int e = errno;
        fail(std::string("madvise DONTNEED: ") + std::strerror(e));
        return false;
    }
    return true;
}

bool StorageMemory::advise_random() {
    if (base_ == nullptr) {
        fail("advise_random: no mapping");
        return false;
    }
    if (::madvise(base_, size_, MADV_RANDOM) != 0) {
        const int e = errno;
        fail(std::string("madvise RANDOM: ") + std::strerror(e));
        return false;
    }
    return true;
}

size_t StorageMemory::resident_pages(uint64_t off, size_t len) const {
    if (base_ == nullptr || off >= size_ || len == 0) return 0;
    const size_t ps       = page_size();
    const uint64_t start  = round_down_to_page64(off);
    const uint64_t end_cl = std::min<uint64_t>(round_up_to_page(off + len), size_);
    const size_t  count   = static_cast<size_t>((end_cl - start) / ps);

    std::vector<unsigned char> vec(count);
    if (::mincore(base_ + start, count * ps, vec.data()) != 0) return 0;

    size_t resident = 0;
    for (const unsigned char v : vec)
        if (v & 1) ++resident;
    return resident;
}

StorageMemory::Stats StorageMemory::stats() const {
    Stats s {};
    s.page_bytes = page_size();
    s.size_bytes = size_;
    if (base_ == nullptr) {
        s.pages = 0;
        s.resident_pages = 0;
        s.resident_bytes = 0;
        return s;
    }
    s.pages          = size_ / s.page_bytes;
    s.resident_pages = resident_pages(0, size_);
    s.resident_bytes = s.resident_pages * s.page_bytes;
    return s;
}

}  // namespace hakios
