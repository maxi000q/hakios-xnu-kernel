#pragma once

// The 'comp' prelinked-kernel wrapper and its LZSS payload -- stage 4 of the
// app edition roadmap.
//
// Once an IMG3 kernel cache is decrypted, its DATA payload is (on iOS 6)
// a compressed prelinked kernel: a small header followed by LZSS-compressed
// bytes that decompress to a fat or thin Mach-O whose armv7 slice is the
// kernel. The header follows Apple's PrelinkedKernelHeader from the
// open-source XNU (kext_tools), all fields little-endian on ARM:
//
//   uint32 signature        'comp'
//   uint32 compressType     'lzss' (iOS 6; later systems grew 'lzvn'/'bvx2')
//   uint32 adler32          of the *uncompressed* data -- the self-check
//   uint32 uncompressedSize
//   uint32 compressedSize   bytes of LZSS stream that follow the header
//   uint32 prelinkVersion
//   uint32 reserved[10]
//   char   platformName[16]   unused on iOS 6
//   char   rootPath[256]      unused on iOS 6
//   uint8  data[]
//
// Two of those fields were corrected against Apple's own file -- the
// kernelcache.release.n90 of the real 6.1.3 (10B329, iPhone3,1) IPSW,
// decrypted and decoded during this stage (see sysimage/ios6/README.md):
//
//   * the stream starts at byte 384 (0x180), not at the struct's 336
//     (0x150): Apple's 2013 kextcache pads the platform area by 48 more
//     zero bytes. Both generators here write 384 to match the real file.
//   * uncompressedSize does NOT bound a prelinked cache's stream: the real
//     file promises 6,343,680 (the pre-prelink kernel) and the stream
//     actually produces 13,393,815 bytes -- kernel + kexts in __PRELINK_TEXT
//     + the kext plists in __PRELINK_INFO. The decoder therefore runs to
//     the end of the stream and requires output >= uncompressedSize; the
//     Mach-O structure downstream is the real integrity gate.
//
// The adler32 field is enforced only when the output is exactly the promised
// size (our own generated kernels): on the real prelinked file the field
// matches no adler of any natural range, so it is recorded, not trusted.
//
// The LZSS variant (parameters confirmed against the reference decompressor
// the community derived from Apple's published code): a 4096-byte ring
// buffer initialised to spaces with the write pointer at 4096 - 18; one flag
// byte per eight tokens, consumed LSB first; a set bit means "one literal
// byte follows", a clear bit means "a two-byte match follows" encoding an
// absolute 12-bit position in the ring (offset = b0 | (b1 & 0xF0) << 4) and
// a length of 3 + (b1 & 0xF). Matches copy byte by byte, so a match may
// overlap itself -- run-length behaviour falls out for free.
//
// The decompression is checked: the output size must reach uncompressedSize
// exactly, and the adler32 of the output must equal the header's. A wrong
// key, a truncated download or a format drift fails loudly here instead of
// putting garbage into guest memory.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hakios/platform.h"

namespace hakios {

class KernelLzss {
public:
    // Apple stores these two FORWARD in the decrypted payload -- 'complzss'
    // reads left to right in a hex editor of a real cache -- unlike the IMG3
    // tag magics of the very same file, which are stored reversed ("3gmI").
    // Two tools, two conventions; both measured on the real 6.1.3 kernelcache.
    // The constants are the little-endian u32 of the bytes as they are.
    static constexpr uint32_t kSignature    = 0x706D6F63u;  // 'comp' as stored
    static constexpr uint32_t kCompressLzss = 0x73737A6Cu;  // 'lzss' as stored
    static constexpr size_t   kHeaderSize   = 384;  // 0x180, as Apple's own cache has it
    static constexpr size_t   kRingSize     = 4096;
    static constexpr size_t   kMaxMatch     = 18;
    static constexpr size_t   kMaxOutput    = size_t(512) << 20;  // a guest is 512 MiB

    struct Header {
        uint32_t signature = 0;
        uint32_t compress_type = 0;
        uint32_t adler32 = 0;
        uint32_t uncompressed_size = 0;
        uint32_t compressed_size = 0;
        uint32_t prelink_version = 0;
        bool     adler_verified = false;  // set by decompress; advisory on
                                         // prelinked caches (see above)
    };

    // True when the bytes start with a 'comp' header naming LZSS.
    static bool is_compressed(const uint8_t* bytes, size_t len);

    // Parse the 384-byte header. Bounds-checked; the signature and the
    // compress type must hold. compressedSize is advisory: Apple's own
    // 6.1.3 cache carries 0x5c1e7100 there, which is not a length, so a
    // field that does not fit is ignored and the stream is the rest of
    // the payload.
    static bool parse_header(const uint8_t* bytes, size_t len, Header* out, std::string* err);

    // Decompress the LZSS stream of a 'comp' blob. The stream runs to its
    // end (the compressedSize field when it is sane, the rest of the input
    // when it is not); the output must reach at least uncompressedSize, and
    // the adler32 is enforced when the output is exactly that size. Output
    // beyond 512 MiB is refused (a guest is 512 MiB of RAM; nothing
    // legitimate is bigger).
    static bool decompress(const uint8_t* bytes, size_t len,
                           std::vector<uint8_t>* out, std::string* err);

    // zlib-compatible adler32 (the format's checksum), for the tests.
    static uint32_t adler32(const uint8_t* p, size_t n);
};

}  // namespace hakios
