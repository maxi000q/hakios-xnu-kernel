#include "hakios/macho_arm.h"

namespace hakios {

namespace {

uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

}  // namespace

bool MachOArm::parse(const uint8_t* bytes, size_t len, std::string* err) {
    valid_ = false;
    was_fat_ = false;
    segments_.clear();
    has_thread_ = false;
    cputype_ = cpusubtype_ = filetype_ = 0;
    entry_pc_ = entry_sp_ = entry_cpsr_ = 0;

    if (len < 4) {
        if (err) *err = "macho: shorter than any header";
        return false;
    }
    const uint32_t first = load_u32(bytes);
    if (first == kMagicFat || first == kMagicFatSw)
        return parse_fat(bytes, len, first, err);
    return parse_thin(bytes, len, err);
}

bool MachOArm::parse_fat(const uint8_t* bytes, size_t len, uint32_t magic, std::string* err) {
    if (len < 8) {
        if (err) *err = "fat: no room for the arch count";
        return false;
    }
    const bool big_endian = (magic == kMagicFat);
    const uint32_t narch = big_endian ? be32(bytes + 4) : load_u32(bytes + 4);
    if (narch == 0 || narch > 16) {
        if (err) *err = "fat: implausible architecture count " + std::to_string(narch);
        return false;
    }
    if (len < 8 + size_t(narch) * 20) {
        if (err) *err = "fat: arch list runs off the end";
        return false;
    }
    for (uint32_t i = 0; i < narch; i++) {
        const uint8_t* a = bytes + 8 + size_t(i) * 20;
        const uint32_t cputype = big_endian ? be32(a) : load_u32(a);
        const uint32_t cpusub  = big_endian ? be32(a + 4) : load_u32(a + 4);
        const uint32_t offset  = big_endian ? be32(a + 8) : load_u32(a + 8);
        const uint32_t size    = big_endian ? be32(a + 12) : load_u32(a + 12);
        if (cputype != kCpuTypeArm || (cpusub != kCpuSubtypeV7 && cpusub != kCpuSubtypeV7S))
            continue;
        if (offset >= len || size > len - offset) {
            if (err) *err = "fat: armv7 slice runs off the end";
            return false;
        }
        was_fat_ = true;
        return parse_thin(bytes + offset, size, err);
    }
    if (err) *err = "fat: no armv7 slice (this build does not emulate arm64/armv6)";
    return false;
}

bool MachOArm::parse_thin(const uint8_t* bytes, size_t len, std::string* err) {
    const auto fail = [err](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };
    if (len < 28) return fail("macho: shorter than the 28-byte header");
    const uint32_t magic = load_u32(bytes);
    if (magic == kMagicSwapped)
        return fail("macho: big-endian Mach-O -- not an iOS image");
    if (magic != kMagic)
        return fail("macho: not a 32-bit Mach-O (no 0xFEEDFACE)");
    cputype_    = load_u32(bytes + 4);
    cpusubtype_ = load_u32(bytes + 8);
    filetype_   = load_u32(bytes + 12);
    const uint32_t ncmds      = load_u32(bytes + 16);
    const uint32_t sizeofcmds = load_u32(bytes + 20);
    if (cputype_ != kCpuTypeArm)
        return fail("macho: cputype " + std::to_string(cputype_) + " is not ARM");
    if (cpusubtype_ != kCpuSubtypeV7 && cpusubtype_ != kCpuSubtypeV7S)
        return fail("macho: cpusubtype " + std::to_string(cpusubtype_) +
                    " is not armv7/armv7s");
    if (filetype_ != kFiletypeKernel && filetype_ != kFiletypeExecute)
        return fail("macho: filetype " + std::to_string(filetype_) +
                    " is neither a kernel nor an executable");
    if (ncmds == 0 || ncmds > 4096)
        return fail("macho: implausible load-command count " + std::to_string(ncmds));
    if (sizeofcmds > len - 28)
        return fail("macho: load commands run off the end of the image");

    size_t off = 28;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (off + 8 > 28 + size_t(sizeofcmds))
            return fail("macho: load command " + std::to_string(i) + " header runs off the end");
        const uint32_t cmd = load_u32(bytes + off);
        const uint32_t cmdsize = load_u32(bytes + off + 4);
        if (cmdsize < 8 || off + cmdsize > 28 + size_t(sizeofcmds))
            return fail("macho: load command " + std::to_string(i) + " runs off the end");
        const uint8_t* lc = bytes + off;

        if (cmd == kCmdSegment) {
            if (cmdsize < 56) return fail("macho: short LC_SEGMENT");
            Segment seg;
            std::memcpy(seg.name, lc + 8, 16);
            seg.name[16] = 0;
            seg.vmaddr   = load_u32(lc + 24);
            seg.vmsize   = load_u32(lc + 28);
            seg.fileoff  = load_u32(lc + 32);
            seg.filesize = load_u32(lc + 36);
            seg.initprot = load_u32(lc + 44);
            if (seg.filesize > 0) {
                if (seg.fileoff >= len || seg.filesize > len - seg.fileoff)
                    return fail("macho: segment " + std::string(seg.name) +
                                " data runs off the end of the image");
                seg.file_bytes = bytes + seg.fileoff;
            }
            if (seg.vmsize < seg.filesize)
                return fail("macho: segment " + std::string(seg.name) +
                            " claims more file bytes than virtual bytes");
            segments_.push_back(seg);
        } else if (cmd == kCmdUnixthread) {
            if (cmdsize < 16 + 4 * kThreadWords) return fail("macho: short LC_UNIXTHREAD");
            const uint32_t flavor = load_u32(lc + 8);
            const uint32_t count  = load_u32(lc + 12);
            if (flavor == kThreadStateArm && count == kThreadWords) {
                entry_pc_   = load_u32(lc + 16 + 4 * 15);
                entry_cpsr_ = load_u32(lc + 16 + 4 * 16);
                entry_sp_   = load_u32(lc + 16 + 4 * 13);
                has_thread_ = true;
            }
        }
        off += cmdsize;
    }

    if (segments_.empty())
        return fail("macho: no LC_SEGMENT load commands");
    valid_ = true;
    return true;
}

const MachOArm::Segment* MachOArm::find_segment(const char* name) const {
    for (const Segment& s : segments_) {
        if (std::strncmp(s.name, name, 16) == 0) return &s;
    }
    return nullptr;
}

}  // namespace hakios
