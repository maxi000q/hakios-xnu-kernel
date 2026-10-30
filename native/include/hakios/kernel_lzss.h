#pragma once

// The 'comp' prelinked-kernel wrapper and its LZSS payload -- stage 4 of the
// app edition roadmap.
//
// Once an IMG3 kernel cache is decrypted, its DATA payload is (on iOS 6)
// a compressed prelinked kernel: a small header followed by LZSS-compressed
// bytes that decompress to a fat Mach-O whose single slice is the kernel.
// The header is Apple's PrelinkedKernelHeader from the open-source XNU
// (kext_tools), all fields little-endian on ARM:
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
//   uint8  data[]            at byte 336 (0x150)
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
    static constexpr uint32_t kSignature    = fourcc("comp");
    static constexpr uint32_t kCompressLzss = fourcc("lzss");
    static constexpr size_t   kHeaderSize   = 336;  // the struct above
    static constexpr size_t   kRingSize     = 4096;
    static constexpr size_t   kMaxMatch     = 18;

    struct Header {
        uint32_t signature = 0;
        uint32_t compress_type = 0;
        uint32_t adler32 = 0;
        uint32_t uncompressed_size = 0;
        uint32_t compressed_size = 0;
        uint32_t prelink_version = 0;
    };

    // True when the bytes start with a 'comp' header naming LZSS.
    static bool is_compressed(const uint8_t* bytes, size_t len);

    // Parse the 336-byte header. Bounds-checked; compressed_size must fit.
    static bool parse_header(const uint8_t* bytes, size_t len, Header* out, std::string* err);

    // Decompress the LZSS stream of a 'comp' blob. Verifies the output size
    // and the adler32, and refuses uncompressedSize beyond 512 MiB (a guest
    // is 512 MiB of RAM; nothing legitimate is bigger).
    static bool decompress(const uint8_t* bytes, size_t len,
                           std::vector<uint8_t>* out, std::string* err);

    // zlib-compatible adler32 (the format's checksum), for the tests.
    static uint32_t adler32(const uint8_t* p, size_t n);
};

}  // namespace hakios
