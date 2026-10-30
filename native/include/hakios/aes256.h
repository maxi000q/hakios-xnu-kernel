#pragma once

// AES-256 CBC decryption -- the one crypto primitive the iOS 6 kernel loader
// needs (stage 4 of the app edition roadmap).
//
// The kernel cache of every iOS 6 IPSW is stored inside its IMG3 container
// AES-256-CBC encrypted; the loader decrypts it with the per-file key and IV
// that the user supplies from their own research (docs/ios6-retarget.md,
// sysimage/ios6/README.md). This project ships no keys and breaks no
// protection: AES is a public standard (FIPS 197) and the key comes from
// outside.
//
// Deliberately small and dependency-free: the Android NDK has no OpenSSL, and
// a kernel loader decrypts ~7 MB once per install -- correctness and audit
// simplicity beat throughput here. The S-box and its inverse are computed from
// the FIPS 197 definition (multiplicative inverse in GF(2^8) under the
// affine transform) instead of being transcribed, so the tables cannot drift
// from the standard. The desktop test rig checks the result against the
// NIST SP 800-38A F.2.5 known-answer vectors.

#include <cstddef>
#include <cstdint>

namespace hakios {

class Aes256Cbc {
public:
    static constexpr size_t kKeyBytes = 32;  // AES-256
    static constexpr size_t kIvBytes  = 16;
    static constexpr size_t kBlock    = 16;

    // Decrypt len bytes of CBC ciphertext (len must be a multiple of kBlock;
    // that is a property of IMG3 DATA payloads, which Apple pads to the block
    // size). in and out may alias. Returns false only on a length that is not
    // a whole number of blocks; wrong keys decrypt to garbage, which the
    // caller's next format check (the 'comp'/Mach-O magic) reports loudly.
    static bool decrypt(const uint8_t key[kKeyBytes],
                        const uint8_t iv[kIvBytes],
                        const uint8_t* in,
                        uint8_t* out,
                        size_t len);
};

}  // namespace hakios
