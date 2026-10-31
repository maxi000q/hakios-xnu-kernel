#include "hakios/kernel_loader.h"

#include <cstdio>

#include "hakios/aes256.h"
#include "hakios/guest_map.h"
#include "hakios/img3.h"
#include "hakios/kernel_lzss.h"

namespace hakios {

std::string KernelLoadReport::describe() const {
    std::string out;
    if (had_img3) out += "IMG3 -> ";
    if (was_decrypted) out += "AES-256-CBC -> ";
    if (was_compressed) out += "comp/LZSS -> ";
    if (was_fat) out += "fat -> ";
    out += "Mach-O armv7, entry 0x";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", entry_pc);
    out += buf;
    out += ", " + std::to_string(placed.size()) + " segments, " +
           std::to_string(bytes_copied) + " bytes into guest RAM";
    return out;
}

bool KernelLoader::load(StorageMemory& guest, const uint8_t* bytes, size_t len,
                        const Key* key, KernelLoadReport* report, std::string* err) {
    const auto fail = [err](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };

    // -- 1. IMG3 container (optional: a bare image goes straight to step 3) --
    const uint8_t* payload = bytes;
    size_t payload_len = len;
    std::vector<uint8_t> decrypted;   // keeps the plaintext of step 2 alive
    Img3 img3;
    if (len >= 4 && load_u32(bytes) == Img3::kMagic) {
        if (!img3.parse(bytes, len, err)) return false;
        report->had_img3 = true;
        report->was_encrypted = img3.kbag(1) != nullptr || img3.kbag(2) != nullptr;
        payload = img3.data(&payload_len);
        if (!payload) return fail("kernel: the IMG3 file carries no DATA tag");

        // -- 2. decryption, with the caller's key, only when there is one ---
        if (report->was_encrypted) {
            if (!key)
                return fail("kernel: this kernel cache is AES-256 encrypted "
                            "(its IMG3 has a KBAG tag) and no key was supplied; "
                            "the app asks for the key during install, and "
                            "nothing is decrypted without it");
            // Apple stores the logical size in the DATA tag; the ciphertext
            // itself is whole 16-byte blocks (measured on the real 6.1.3
            // kernelcache: 7,413,724 promised, 7,413,712 decryptable). The
            // tail of the last partial block is not in the file at all, and
            // every stage below is bounded by its own sizes, so it is
            // dropped rather than guessed at.
            if (payload_len % Aes256Cbc::kBlock != 0)
                payload_len -= payload_len % Aes256Cbc::kBlock;
            if (payload_len == 0)
                return fail("kernel: encrypted payload is shorter than one "
                            "AES block -- corrupt download?");
            decrypted.resize(payload_len);
            if (!Aes256Cbc::decrypt(key->key, key->iv, payload, decrypted.data(), payload_len))
                return fail("kernel: AES decryption rejected the payload length");
            payload = decrypted.data();
            report->was_decrypted = true;
        }
    }

    // -- 3. 'comp' LZSS wrapper (optional) --------------------------------
    std::vector<uint8_t> inflated;
    if (KernelLzss::is_compressed(payload, payload_len)) {
        if (!KernelLzss::decompress(payload, payload_len, &inflated, err)) return false;
        payload = inflated.data();
        payload_len = inflated.size();
        report->was_compressed = true;
    }

    // -- 4. Mach-O ---------------------------------------------------------
    MachOArm mach;
    if (!mach.parse(payload, payload_len, err)) {
        if (report->had_img3 && !report->was_encrypted && !report->was_compressed) {
            // The two most likely explanations, in order, for a bare payload
            // that is neither 'comp' nor Mach-O: it is actually encrypted
            // (some tools strip the KBAG) or it is not a kernel at all.
            return fail(std::string(*err) + " -- the payload is neither a "
                        "compressed nor a raw kernel image; if this file came "
                        "from an IPSW it is probably encrypted and needs a key");
        }
        return false;
    }
    report->was_fat = mach.was_fat();

    // -- 5. segments into the guest ----------------------------------------
    for (const MachOArm::Segment& seg : mach.segments()) {
        // Apple's prelinked caches carry zero-size marker segments (__LAST,
        // __PRELINK_STATE): they cover no bytes and place nothing.
        if (seg.vmsize == 0) {
            if (report) {
                KernelLoadReport::Placed p;
                std::snprintf(p.name, sizeof p.name, "%s", seg.name);
                p.vmaddr = seg.vmaddr; p.vmsize = 0; p.filesize = 0;
                report->placed.push_back(p);
            }
            continue;
        }
        const uint64_t from = uint64_t(seg.vmaddr);
        const uint64_t to   = uint64_t(seg.vmaddr) + uint64_t(seg.vmsize);
        if (to <= from || from < map::kRamBase || to > map::kRamEnd)
            return fail("kernel: segment " + std::string(seg.name) +
                        " covers [0x" + std::to_string(from) + ", 0x" +
                        std::to_string(to) + "), outside the emulated iPhone's "
                        "RAM at [0x80000000, 0xA0000000) -- this kernel was "
                        "not built for this machine");
        if (seg.filesize > 0 &&
            !guest.copy_in(from, seg.file_bytes, seg.filesize))
            return fail("kernel: copying segment " + std::string(seg.name) +
                        " into guest memory failed: " + guest.last_error());
        if (seg.vmsize > seg.filesize) {  // the BSS tail: zeros, once
            const size_t zero_len = seg.vmsize - seg.filesize;
            std::vector<uint8_t> zeros(zero_len, 0);  // a segment-sized zero
            if (!guest.copy_in(from + seg.filesize, zeros.data(), zero_len))
                return fail("kernel: zero-filling segment " + std::string(seg.name) +
                            " failed: " + guest.last_error());
        }
        if (report) {
            KernelLoadReport::Placed p;
            std::snprintf(p.name, sizeof p.name, "%s", seg.name);
            p.vmaddr = seg.vmaddr; p.vmsize = seg.vmsize; p.filesize = seg.filesize;
            report->placed.push_back(p);
        }
        report->bytes_copied += seg.filesize;
    }

    // Keep the hot pages: the interpreter is about to fetch from __TEXT.
    if (const MachOArm::Segment* text = mach.find_segment("__TEXT"))
        guest.advise_willneed(text->vmaddr, text->filesize);

    if (mach.has_thread()) {
        report->entry_pc = mach.entry_pc();
        report->entry_sp = mach.entry_sp();
    } else if (!mach.segments().empty()) {
        report->entry_pc = mach.segments().front().vmaddr;  // loader's fallback
    }
    return true;
}

}  // namespace hakios
