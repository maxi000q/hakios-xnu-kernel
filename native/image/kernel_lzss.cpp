#include "hakios/kernel_lzss.h"

namespace hakios {

bool KernelLzss::is_compressed(const uint8_t* bytes, size_t len) {
    Header h;
    if (len < kHeaderSize) return false;
    if (!parse_header(bytes, len, &h, nullptr)) return false;
    return h.compress_type == kCompressLzss;
}

bool KernelLzss::parse_header(const uint8_t* bytes, size_t len, Header* out, std::string* err) {
    const auto fail = [err](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };
    if (len < kHeaderSize)
        return fail("comp: shorter than the 384-byte prelinked-kernel header");
    Header h;
    h.signature         = load_u32(bytes);
    h.compress_type     = load_u32(bytes + 4);
    h.adler32           = load_u32(bytes + 8);
    h.uncompressed_size = load_u32(bytes + 12);
    h.compressed_size   = load_u32(bytes + 16);
    h.prelink_version   = load_u32(bytes + 20);
    if (h.signature != kSignature)
        return fail("comp: signature is not 'comp' -- either not a prelinked "
                    "kernel or the decryption key was wrong");
    if (h.compress_type != kCompressLzss)
        return fail("comp: compressType 0x" + std::to_string(h.compress_type) +
                    " is not LZSS (iOS 10+ 'lzvn' caches are out of scope)");
    if (out) *out = h;
    return true;
}

uint32_t KernelLzss::adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

bool KernelLzss::decompress(const uint8_t* bytes, size_t len,
                            std::vector<uint8_t>* out, std::string* err) {
    out->clear();
    const auto fail = [err](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };
    Header h;
    if (!parse_header(bytes, len, &h, err)) return false;
    if (h.uncompressed_size == 0 || h.uncompressed_size > kMaxOutput)
        return fail("comp: uncompressedSize " + std::to_string(h.uncompressed_size) +
                    " is not a plausible kernel size (limit 512 MiB)");
    out->reserve(h.uncompressed_size);

    // The stream's end: the header's compressedSize when it is a sane
    // length, the rest of the input when it is not -- Apple's own 6.1.3
    // cache carries 0x5c1e7100 in that field, which is not a length.
    size_t stream_len = len - kHeaderSize;
    if (h.compressed_size != 0 && h.compressed_size <= stream_len)
        stream_len = h.compressed_size;

    // The ring: 4096 bytes of spaces, write position 4096 - 18, exactly as
    // the reference decoder initializes it.
    uint8_t ring[kRingSize];
    std::memset(ring, 0x20, sizeof ring);
    size_t dictptr = kRingSize - kMaxMatch;

    const uint8_t* src = bytes + kHeaderSize;
    const uint8_t* end = src + stream_len;
    uint32_t flags = 0;
    int bits = 0;
    // The loop runs to the stream's end, not to uncompressedSize: a real
    // prelinked cache decompresses to MORE than the header's size field
    // (the kexts and their plists ride behind the kernel in the stream).
    while (src < end) {
        if (bits == 0) {
            flags = *src++;
            bits = 8;
        }
        if (flags & 1u) {                       // literal
            ring[dictptr] = *src;
            out->push_back(*src++);
            dictptr = (dictptr + 1) % kRingSize;
        } else {                                // match: two more bytes
            if (end - src < 2)
                return fail("comp: LZSS stream ends inside a match");
            const size_t pos = size_t(src[0]) | (size_t(src[1] & 0xF0) << 4);
            const size_t count = 3 + (src[1] & 0xF);
            src += 2;
            if (pos >= kRingSize)
                return fail("comp: match position beyond the ring");
            for (size_t i = 0; i < count; i++) {
                const uint8_t c = ring[(pos + i) % kRingSize];  // byte-by-byte:
                ring[dictptr] = c;                              // a match may read
                out->push_back(c);                              // what it wrote
                dictptr = (dictptr + 1) % kRingSize;
            }
        }
        flags >>= 1;
        bits--;
        if (out->size() > kMaxOutput)
            return fail("comp: LZSS stream inflated past 512 MiB");
    }

    if (out->size() < h.uncompressed_size)
        return fail("comp: LZSS stream produced " + std::to_string(out->size()) +
                    " bytes, the header promises at least " +
                    std::to_string(h.uncompressed_size));
    const uint32_t got = adler32(out->data(), out->size());
    h.adler_verified = (got == h.adler32);
    // The checksum is enforced exactly when the field describes the whole
    // output (our generated kernels). Apple's real prelinked cache promises
    // the pre-prelink kernel size while the stream yields kernel + kexts +
    // plists, and its field matches no adler of any natural range -- there
    // the Mach-O structure downstream is the integrity gate, not this word.
    if (out->size() == h.uncompressed_size && !h.adler_verified)
        return fail("comp: adler32 mismatch (got 0x" + std::to_string(got) +
                    ", the header says 0x" + std::to_string(h.adler32) +
                    ") -- corrupt data or a wrong decryption key");
    return true;
}

}  // namespace hakios
