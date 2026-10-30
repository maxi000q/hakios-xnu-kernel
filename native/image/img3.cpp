#include "hakios/img3.h"

#include <cstdio>

namespace hakios {

bool Img3::parse(const uint8_t* bytes, size_t len, std::string* err) {
    valid_ = false;
    type_ = data_ = Tag{};
    kbag_prod_ = kbag_dev_ = Kbag{};
    has_sepo_ = has_chip_ = has_bord_ = has_ecid_ = false;

    const auto fail = [err](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };
    if (len < 20) return fail("img3: shorter than the 20-byte header");
    if (load_u32(bytes) != kMagic)
        return fail("img3: not an IMG3 file (first four bytes are not '3gmI')");
    full_    = load_u32(bytes + 4);
    no_pack_ = load_u32(bytes + 8);
    ident_   = load_u32(bytes + 16);
    if (full_ != len)
        return fail("img3: header says " + std::to_string(full_) +
                    " bytes, the buffer holds " + std::to_string(len));

    size_t off = 20;
    while (off < len) {
        if (len - off < 12)
            return fail("img3: tag header at " + std::to_string(off) + " runs off the end");
        const uint32_t magic = load_u32(bytes + off);
        const uint32_t total = load_u32(bytes + off + 4);
        const uint32_t dlen  = load_u32(bytes + off + 8);
        if (total < 12 || dlen > total - 12)
            return fail("img3: tag lengths disagree (total " + std::to_string(total) +
                        ", data " + std::to_string(dlen) + ")");
        if (total > len - off)
            return fail("img3: tag runs off the end of the file");
        const uint8_t* payload = bytes + off + 12;

        Tag tag{magic, payload, dlen};
        if (magic == kTagType && !type_.data) {
            type_ = tag;
        } else if (magic == kTagData && !data_.data) {
            data_ = tag;
        } else if (magic == kTagKbag) {
            // cryptState, aesType, then IV[16] and the key (16/24/32 bytes).
            if (dlen >= 8) {
                Kbag k;
                k.crypt_state = load_u32(payload);
                k.aes_type    = load_u32(payload + 4);
                const size_t key_len = k.aes_type == 0x100 ? 32 :
                                       k.aes_type == 0x80  ? 16 : 24;
                if (dlen >= 8 + 16 + key_len) {
                    std::memcpy(k.iv, payload + 8, 16);
                    std::memcpy(k.key, payload + 8 + 16, key_len);
                    k.key_len = key_len;
                    if (k.crypt_state == 1) kbag_prod_ = k;
                    if (k.crypt_state == 2) kbag_dev_  = k;
                }
            }
        } else if (magic == kTagSepo && dlen >= 4) {
            sepo_ = load_u32(payload); has_sepo_ = true;
        } else if (magic == kTagChip && dlen >= 4) {
            chip_ = load_u32(payload); has_chip_ = true;
        } else if (magic == kTagBord && dlen >= 4) {
            bord_ = load_u32(payload); has_bord_ = true;
        } else if (magic == kTagEcid && dlen >= 4) {
            ecid_ = load_u32(payload); has_ecid_ = true;
        }
        off += total;
    }

    if (!data_.data) return fail("img3: no DATA tag");
    valid_ = true;
    return true;
}

const uint8_t* Img3::data(size_t* len) const {
    if (!valid_) {
        if (len) *len = 0;
        return nullptr;
    }
    if (len) *len = data_.data_len;
    return data_.data;
}

const Img3::Kbag* Img3::kbag(uint32_t crypt_state) const {
    if (!valid_) return nullptr;
    if (crypt_state == 1 && kbag_prod_.key_len) return &kbag_prod_;
    if (crypt_state == 2 && kbag_dev_.key_len) return &kbag_dev_;
    return nullptr;
}

bool Img3::word_tag(uint32_t magic, uint32_t* out) const {
    if (!valid_) return false;
    if (magic == kTagSepo && has_sepo_) { if (out) *out = sepo_; return true; }
    if (magic == kTagChip && has_chip_) { if (out) *out = chip_; return true; }
    if (magic == kTagBord && has_bord_) { if (out) *out = bord_; return true; }
    if (magic == kTagEcid && has_ecid_) { if (out) *out = ecid_; return true; }
    return false;
}

std::string Img3::describe() const {
    if (!valid_) return "img3: not parsed";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", ident_);
    std::string out = "IMG3 ident 0x" + std::string(buf);
    size_t dlen = 0;
    data(&dlen);
    out += ", DATA " + std::to_string(dlen) + " bytes";
    if (has_chip_) {
        std::snprintf(buf, sizeof buf, "%x", chip_);
        out += ", CHIP 0x" + std::string(buf);
    }
    if (has_bord_) out += ", BORD " + std::to_string(bord_);
    if (has_sepo_) out += ", SEPO " + std::to_string(sepo_);
    if (kbag_prod_.key_len)
        out += ", encrypted (production KBAG, AES-" +
               std::to_string(kbag_prod_.key_len * 8) + ")";
    else if (kbag_dev_.key_len)
        out += ", encrypted (development KBAG only)";
    else
        out += ", not encrypted (no KBAG)";
    return out;
}

}  // namespace hakios
