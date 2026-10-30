#pragma once

// The armv7 Mach-O parser -- stage 4 of the app edition roadmap.
//
// What an iOS 6 kernel cache decompresses to: a fat ("universal") Mach-O with
// a single armv7 slice, whose load commands are LC_SEGMENTs (the kernel's
// __TEXT, __DATA, ... pieces with their guest virtual addresses) and one
// LC_UNIXTHREAD holding the entry PC. XNU 2107 is 32-bit: addresses fit a
// uint32, page tables are short descriptors, and the kernel's static virtual
// base sits around 0x80001000 -- inside the emulated iPhone's 512 MiB of RAM
// (guest_map.h), which is what makes "segments land in guest memory" a plain
// copy with bounds checks.
//
// Only what a loader needs is parsed: the 32-bit header, fat wrappers,
// LC_SEGMENT and LC_UNIXTHREAD. Nothing is allocated, relocated or linked;
// sections inside segments are skipped by cmdsize, and every offset coming
// from the file is bounds-checked before use (a kernel image is data from
// the internet until proven otherwise).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hakios/platform.h"

namespace hakios {

class MachOArm {
public:
    static constexpr uint32_t kMagic       = 0xFEEDFACE;  // MH_MAGIC, LE read
    static constexpr uint32_t kMagicSwapped = 0xCEFAEDFE; // byte-swapped view
    // A fat file's magic bytes are "CA FE BA BE" with big-endian fields, so a
    // *little-endian* u32 read of them yields 0xBEBAFECA -- and the swapped
    // view is the one that compares equal to the familiar 0xCAFEBABE.
    static constexpr uint32_t kMagicFat    = 0xBEBAFECA;  // bytes CA FE BA BE, BE fields
    static constexpr uint32_t kMagicFatSw  = 0xCAFEBABE;  // bytes BE BA FE CA, LE fields
    static constexpr uint32_t kCpuTypeArm  = 12;
    static constexpr uint32_t kCpuSubtypeV7  = 9;   // armv7: iPhone 4 / iOS 6
    static constexpr uint32_t kCpuSubtypeV7S = 11;  // armv7s: accepted, same ISA from the loader's view
    static constexpr uint32_t kFiletypeKernel  = 11;  // MH_KERNEL
    static constexpr uint32_t kFiletypeExecute = 2;   // MH_EXECUTE

    static constexpr uint32_t kCmdSegment    = 0x1;
    static constexpr uint32_t kCmdUnixthread = 0x5;
    static constexpr uint32_t kThreadStateArm = 1;   // flavor of LC_UNIXTHREAD state
    // arm_thread_state_t: r[13], sp, lr, pc, cpsr -> 17 words; pc is word 15
    static constexpr uint32_t kThreadWords   = 17;

    struct Segment {
        char     name[17] = {0};   // segname, NUL-terminated for printing
        uint32_t vmaddr = 0;
        uint32_t vmsize = 0;
        uint32_t fileoff = 0;
        uint32_t filesize = 0;
        uint32_t initprot = 0;
        const uint8_t* file_bytes = nullptr;  // into the parsed buffer
    };

    // Parse a fat wrapper (taking the first armv7/armv7s slice) or a thin
    // armv7 image. On any inconsistency: false and a message in *err.
    bool parse(const uint8_t* bytes, size_t len, std::string* err);

    bool        valid() const { return valid_; }
    uint32_t    cputype() const { return cputype_; }
    uint32_t    cpusubtype() const { return cpusubtype_; }
    uint32_t    filetype() const { return filetype_; }
    bool        was_fat() const { return was_fat_; }
    const std::vector<Segment>& segments() const { return segments_; }
    const Segment* find_segment(const char* name) const;

    // Entry state from LC_UNIXTHREAD. has_thread() false means the loader
    // falls back to the first segment's vmaddr.
    bool     has_thread() const { return has_thread_; }
    uint32_t entry_pc() const { return entry_pc_; }
    uint32_t entry_sp() const { return entry_sp_; }
    uint32_t entry_cpsr() const { return entry_cpsr_; }

private:
    bool parse_thin(const uint8_t* bytes, size_t len, std::string* err);
    bool parse_fat(const uint8_t* bytes, size_t len, uint32_t magic, std::string* err);

    bool valid_ = false;
    bool was_fat_ = false;
    uint32_t cputype_ = 0, cpusubtype_ = 0, filetype_ = 0;
    std::vector<Segment> segments_;
    bool has_thread_ = false;
    uint32_t entry_pc_ = 0, entry_sp_ = 0, entry_cpsr_ = 0;
};

}  // namespace hakios
