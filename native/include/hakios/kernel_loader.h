#pragma once

// The kernel loader -- stage 4 of the app edition roadmap, the piece that
// makes "segments land in guest memory" true.
//
// One call walks the whole chain an iOS 6 kernel cache throws at you, in
// order, and every layer is optional because our own stage-8 self-test
// kernel exercises the pipeline without Apple's encryption:
//
//   IMG3 unwrap  (native/image/img3)        -- or a bare image, no wrapper
//   AES-256-CBC  (native/image/aes256)      -- only when the caller has a key
//   fat Mach-O   (native/image/macho_arm)   -- one armv7 slice
//   'comp' LZSS  (native/image/kernel_lzss) -- or stored uncompressed
//   Mach-O armv7 (native/image/macho_arm)
//   segments -> StorageMemory               -- copy file bytes, zero-fill BSS
//
// The guest is the stage-2 storage image and the stage-3 interpreter runs
// what lands there; every segment's [vmaddr, vmaddr + vmsize) must fit the
// emulated iPhone's RAM (guest_map.h), which is the one hard rule. A wrong
// or missing key fails at the 'comp'/Mach-O magic check with a message that
// says which step and why -- never a silent bad boot.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hakios/macho_arm.h"
#include "hakios/storage_memory.h"

namespace hakios {

struct KernelLoadReport {
    bool had_img3 = false;        // an IMG3 container was unwrapped
    bool was_encrypted = false;   // IMG3 said so (KBAG present)
    bool was_decrypted = false;   // the caller's key was used
    bool was_compressed = false;  // a 'comp' LZSS wrapper was inflated
    bool was_fat = false;         // a fat Mach-O was split
    uint32_t entry_pc = 0;
    uint32_t entry_sp = 0;
    struct Placed {
        char     name[17] = {0};
        uint32_t vmaddr = 0;
        uint32_t vmsize = 0;
        uint32_t filesize = 0;
    };
    std::vector<Placed> placed;
    size_t bytes_copied = 0;

    std::string describe() const;
};

class KernelLoader {
public:
    // An AES-256 key + IV for the IMG3 DATA payload, from the user's own
    // research (the app's install screen asks; the desktop rig passes test
    // values). nullptr = no key -- then an encrypted kernel fails with the
    // message that names the missing step.
    struct Key {
        uint8_t iv[16]  = {0};
        uint8_t key[32] = {0};
    };

    // Load a kernel image into the guest. Returns false with *err set on the
    // first layer that does not hold; the report is filled as far as the walk
    // got.
    static bool load(StorageMemory& guest, const uint8_t* bytes, size_t len,
                     const Key* key, KernelLoadReport* report, std::string* err);
};

}  // namespace hakios
