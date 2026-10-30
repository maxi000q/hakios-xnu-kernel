// Stage 2 unit tests: the storage-backed guest memory.
//
// Run: tests/run_desktop_tests.sh   (or link manually with the host g++)
//
// The tests that matter most here prove the two properties the whole app
// edition is built on:
//   1. persistence -- the "RAM" is a file, so it survives close/reopen
//      and process death ("installed stays installed");
//   2. eviction without loss -- MADV_DONTNEED drops pages of a MAP_SHARED
//      file mapping and the data reads back identical (the guest is bounded
//      by storage, not by RAM).

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hakios/guest_map.h"
#include "hakios/platform.h"
#include "hakios/storage_memory.h"

using hakios::StorageMemory;

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static void expect_error(StorageMemory& m, const char* what) {
    if (m.last_error().empty()) {
        std::printf("FAIL: expected an error set by %s\n", what);
        ++g_failures;
    }
}

// Resident size (kB) of the VMA containing addr, out of /proc/self/smaps.
// mincore() reads the page cache, so it cannot show our own process letting
// go of pages; smaps can. -1 when smaps is unavailable (callers skip).
static long vma_rss_kb(const void* addr) {
    std::FILE* f = std::fopen("/proc/self/smaps", "r");
    if (!f) return -1;
    char line[512];
    bool in_target = false;
    const auto target = reinterpret_cast<unsigned long>(addr);
    while (std::fgets(line, sizeof line, f)) {
        unsigned long s = 0, e = 0;
        if (std::sscanf(line, "%lx-%lx", &s, &e) == 2) {
            in_target = (s <= target && target < e);
        } else if (in_target) {
            long rss = 0;
            if (std::sscanf(line, "Rss: %ld", &rss) == 1) {
                std::fclose(f);
                return rss;
            }
        }
    }
    std::fclose(f);
    return -1;
}

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : ".";
    const std::string img = dir + "/physmem.bin";
    ::unlink(img.c_str());

    // -- create -----------------------------------------------------------------
    StorageMemory m;
    CHECK(m.create(img, hakios::map::kPhysSize));
    CHECK(m.valid());
    CHECK(m.size() == hakios::map::kPhysSize);
    CHECK(m.base() != nullptr);

    struct stat st {};
    CHECK(::stat(img.c_str(), &st) == 0);
    CHECK(static_cast<size_t>(st.st_size) == hakios::map::kPhysSize);

    // -- fresh image is zero-filled ----------------------------------------------
    CHECK(m.read32(0) == 0);
    CHECK(m.read32(0x1000) == 0);
    CHECK(m.read32(hakios::map::kVramBase) == 0);
    CHECK(m.read32(hakios::map::kPhysSize - 4) == 0);

    // -- typed accessors, aligned and unaligned ----------------------------------
    m.write32(0x100, 0xDEADBEEF);
    CHECK(m.read32(0x100) == 0xDEADBEEF);
    m.write16(0x104, 0xBEEF);
    CHECK(m.read16(0x104) == 0xBEEF);
    CHECK(m.read8(0x100) == 0xEF);
    CHECK(m.read8(0x103) == 0xDE);
    m.write8(0x105, 0x7F);
    CHECK(m.read8(0x105) == 0x7F);

    // unaligned 32-bit access (memcpy semantics, no alignment requirement)
    m.write32(0x203, 0x01020304);
    CHECK(m.read32(0x203) == 0x01020304);

    // VRAM is ordinary guest memory seen through the same API
    m.write32(hakios::map::kVramBase + 8, 0x11223344);
    CHECK(m.read32(hakios::map::kVramBase + 8) == 0x11223344);

    // -- bounds checking -----------------------------------------------------------
    m.write32(0x100, 0);
    CHECK(m.read32(m.size()) == 0);           // one past the end
    expect_error(m, "read32 past end");
    m.write32(m.size() - 3, 1);                // straddles the end: dropped
    expect_error(m, "write32 straddling end");
    CHECK(m.read8(m.size() - 3) == 0);
    CHECK(m.read8(m.size() - 2) == 0);
    CHECK(m.read32(m.size() - 4) == 0);
    m.write8(m.size() - 1, 1);                // the last byte itself: fine
    CHECK(m.read8(m.size() - 1) == 1);

    // -- bulk copies ----------------------------------------------------------------
    std::vector<uint8_t> blob(64 * 1024);
    for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>(i * 7 + 3);
    CHECK(m.copy_in(0x100000, blob.data(), blob.size()));
    std::vector<uint8_t> back(blob.size(), 0);
    CHECK(m.copy_out(0x100000, back.data(), back.size()));
    CHECK(std::memcmp(blob.data(), back.data(), blob.size()) == 0);

    CHECK(!m.copy_in(m.size() - 8, blob.data(), 16));  // out of range: nothing moves
    expect_error(m, "copy_in out of range");
    std::vector<uint8_t> guard(16, 0xAA);
    CHECK(m.copy_in(0x100000, guard.data(), guard.size()));  // overwrite head
    CHECK(!m.copy_in(m.size() - 8, blob.data(), 16));
    CHECK(m.read32(0x100000) == 0xAAAAAAAA);  // failed copy wrote nothing

    // -- residency: pages really come and go ------------------------------------------
    const uint64_t hot = 0x200000;  // 2 MiB in, page-aligned
    m.write32(hot, 0x12345678);
    m.write32(hot + hakios::page_size(), 0x9ABCDEF0);
    m.write32(hot + 2 * hakios::page_size(), 0x0F0F0F0F);
    const size_t res_before = m.resident_pages(hot, 3 * hakios::page_size());
    CHECK(res_before == 3);

    // The process's own footprint for this mapping, before letting go.
    // (mincore() reads the page cache; smaps reads our PTEs. The distinction
    // is the whole point: the page cache may keep the data, our RSS must not.)
    const long rss_before = vma_rss_kb(m.base());

    CHECK(m.advise_dontneed(hot, 3 * hakios::page_size()));
    const size_t res_after = m.resident_pages(hot, 3 * hakios::page_size());
    CHECK(res_after <= res_before);  // the page cache may still hold them

    if (rss_before >= 0) {
        const long rss_after = vma_rss_kb(m.base());
        CHECK(rss_after >= 0);
        // 3 pages x 4 KiB left this process's RAM when we let them go
        CHECK(rss_after <= rss_before - 12);
        std::printf("info: mapping RSS %ld kB -> %ld kB after MADV_DONTNEED\n", rss_before,
                    rss_after);
    } else {
        std::printf("info: /proc/self/smaps unavailable, RSS check skipped\n");
    }

    // ... and nothing was lost: refault brings identical bytes back
    CHECK(m.read32(hot) == 0x12345678);
    CHECK(m.read32(hot + hakios::page_size()) == 0x9ABCDEF0);
    CHECK(m.read32(hot + 2 * hakios::page_size()) == 0x0F0F0F0F);
    CHECK(m.resident_pages(hot, 3 * hakios::page_size()) >= 1);  // refaulted

    CHECK(m.advise_willneed(hot, hakios::page_size()));  // advisory, may not fault in now
    CHECK(m.advise_random());

    // bad ranges are refused
    CHECK(!m.advise_dontneed(m.size(), 4096));
    CHECK(!m.advise_willneed(0, 0));

    // whole-mapping stats are consistent with sub-ranges
    const StorageMemory::Stats s = m.stats();
    CHECK(s.size_bytes == hakios::map::kPhysSize);
    CHECK(s.page_bytes == hakios::page_size());
    CHECK(s.pages == s.size_bytes / s.page_bytes);
    CHECK(s.resident_pages >= m.resident_pages(hot, 3 * hakios::page_size()));

    // -- sparseness: a 528 MiB image that costs only its touched pages ----------------
    CHECK(::stat(img.c_str(), &st) == 0);
    const double used_mib = st.st_blocks * 512.0 / (1024 * 1024);
    const double logical_mib = st.st_size / (1024.0 * 1024.0);
    std::printf("info: %s logical %.0f MiB, on-flash %.2f MiB\n", img.c_str(), logical_mib,
                used_mib);

    // -- persistence across close/reopen -------------------------------------------------
    m.write32(0x1000, 0xCAFEF00D);
    m.write32(hakios::map::kVramBase + 4, 0x00ABCDEF);
    m.close();
    CHECK(!m.valid());

    StorageMemory m2;
    CHECK(m2.open_existing(img, hakios::map::kPhysSize));
    CHECK(m2.read32(0x1000) == 0xCAFEF00D);
    CHECK(m2.read32(hakios::map::kVramBase + 4) == 0x00ABCDEF);
    CHECK(m2.read32(0x100000) == 0xAAAAAAAA);   // the guard head of the blob
    CHECK(m2.read32(0x100010) == 0x88817A73);  // blob bytes persisted: 73 7A 81 88

    // wrong expected size is rejected
    StorageMemory m3;
    CHECK(!m3.open_existing(img, 123 * 4096));
    CHECK(!m3.valid());

    // two mappings of the same image see each other's writes (MAP_SHARED)
    {
        StorageMemory m4;
        CHECK(m4.open_existing(img, hakios::map::kPhysSize));
        m4.write32(0x2000, 0x55667788);
        CHECK(m2.read32(0x2000) == 0x55667788);
    }

    // -- construction rules ------------------------------------------------------------
    StorageMemory bad;
    CHECK(!bad.create(img, 0));          // zero size
    CHECK(!bad.create(img, 4096 + 1));   // not page-multiple
    CHECK(!bad.open_existing(dir + "/no-such-file.bin"));

    // move semantics hand the mapping over, the source becomes invalid
    StorageMemory moved = std::move(m2);
    CHECK(moved.valid());
    CHECK(moved.read32(0x2000) == 0x55667788);
    CHECK(!m2.valid());
    CHECK(m2.create(dir + "/small.bin", 65536));  // a moved-from object works again
    CHECK(m2.read32(65536 - 4) == 0);              // on a fresh, zeroed image

    if (g_failures == 0) {
        std::printf("test_memory: all checks passed\n");
        return 0;
    }
    std::printf("test_memory: %d failure(s)\n", g_failures);
    return 1;
}
