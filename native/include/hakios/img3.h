#pragma once

// The IMG3 container -- the wrapper of everything bootable in an iOS 6 IPSW
// (stage 4 of the app edition roadmap). Replaces IMG4/IM4P, which is what
// the 16.5-era files of the native path use.
//
// Layout (verified against the kernelcache.release.n90 of the iPhone3,1
// 6.1.3 (10B329) IPSW, Apple's own file, byte for byte):
//
//   uint32 magic          'Img3' (see fourcc below for the byte order)
//   uint32 fullSize       the whole file
//   uint32 sizeNoPack     everything minus this 20-byte header
//   uint32 sigCheckArea   start of DATA up to the SHSH tag
//   uint32 ident          what the file is ('krnl' for a kernel cache)
//   tags[]                TYPE, DATA, SEPO, KBAG, SHSH, CERT, ...
//
//   tag: uint32 magic; uint32 totalLength (incl. the 12-byte tag header and
//   the padding); uint32 dataLength; data[pad to 4]
//
// Apple's tools write every four-character magic so that it reads backwards
// in a hex editor: the first four bytes of a 6.1.3 kernel cache are "3gmI",
// the TYPE tag appears as "EPYT". Reading those four bytes as a
// little-endian u32 gives exactly the fourcc constant ('I'<<24|'m'<<16|
// 'g'<<8|'3'), which is how every magic is compared here.
//
// Encryption: a kernel cache with a KBAG tag has its DATA payload
// AES-256-CBC encrypted. The KBAG itself holds the IV and key wrapped with
// the device group (GID) key, which no emulator has -- the *unwrapped* IV and
// key are published per firmware image by the research community, and this
// loader expects them from the caller (the app asks the user; the desktop
// rig passes them in a test). Nothing is wrapped, unwrapped or attacked
// here: AES with a caller-supplied key only.

#include <cstddef>
#include <cstdint>
#include <string>

#include "hakios/platform.h"

namespace hakios {

class Img3 {
public:
    static constexpr uint32_t kMagic     = fourcc("Img3");
    // tags a kernel loader cares about
    static constexpr uint32_t kTagType   = fourcc("TYPE");
    static constexpr uint32_t kTagData   = fourcc("DATA");
    static constexpr uint32_t kTagKbag   = fourcc("KBAG");
    static constexpr uint32_t kTagSepo   = fourcc("SEPO");
    static constexpr uint32_t kTagChip   = fourcc("CHIP");
    static constexpr uint32_t kTagBord   = fourcc("BORD");
    static constexpr uint32_t kTagEcid   = fourcc("ECID");
    static constexpr uint32_t kTagVers   = fourcc("VERS");
    static constexpr uint32_t kTagShsh   = fourcc("SHSH");
    static constexpr uint32_t kTagCert   = fourcc("CERT");
    // idents (the TYPE payload, and the header's ident field)
    static constexpr uint32_t kIdentKernel = fourcc("krnl");

    // A KBAG, as stored: cryptState 1 = production, 2 = development;
    // aesType 0x80 = AES-128, 0x100 = AES-256; then IV[16] and key[32] (or
    // key[24]/key[16] for the smaller variants), all wrapped with the GID key
    // of the device family the file is for.
    struct Kbag {
        uint32_t crypt_state = 0;
        uint32_t aes_type    = 0;
        uint8_t  iv[16]      = {0};
        uint8_t  key[32]     = {0};
        size_t   key_len     = 0;    // 16, 24 or 32 bytes, per aes_type
    };

    // Walk the header and the tag list. Every length is bounds-checked against
    // the input; a file that runs off the end, a tag that overlaps the next,
    // or a magic that is not Img3 fails with a message in *err. A parse
    // leaves the object empty (safe to reuse).
    bool parse(const uint8_t* bytes, size_t len, std::string* err);

    bool        valid() const { return valid_; }
    uint32_t    full_size() const { return full_; }
    uint32_t    size_no_pack() const { return no_pack_; }
    uint32_t    ident() const { return ident_; }

    // The DATA payload: the (possibly encrypted, possibly compressed) kernel
    // image. Returns nullptr with *len == 0 when there is none.
    const uint8_t* data(size_t* len) const;

    // First KBAG with the given crypt state (1 production, 2 development),
    // or nullptr when the file carries none -- which means the DATA payload
    // is stored in the clear (our own stage-8 self-test kernel is one).
    const Kbag* kbag(uint32_t crypt_state) const;

    // The u32 payload of a tag that holds one (SEPO, CHIP, BORD, ECID...).
    bool word_tag(uint32_t magic, uint32_t* out) const;

    // For reports: "TYPE krnl, DATA 7413724 bytes, SEPO 2, CHIP 0x8930,
    // encrypted (production KBAG, AES-256)".
    std::string describe() const;

private:
    struct Tag {
        uint32_t        magic = 0;
        const uint8_t*  data  = nullptr;
        uint32_t        data_len = 0;
    };
    bool valid_ = false;
    uint32_t full_ = 0, no_pack_ = 0, ident_ = 0;
    Tag type_, data_;
    Kbag kbag_prod_, kbag_dev_;
    uint32_t sepo_ = 0, chip_ = 0, bord_ = 0, ecid_ = 0;
    bool has_sepo_ = false, has_chip_ = false, has_bord_ = false, has_ecid_ = false;
};

}  // namespace hakios
