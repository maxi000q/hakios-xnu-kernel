#include "hakios/aes256.h"

#include "hakios/platform.h"

namespace hakios {

namespace {

// -- GF(2^8) arithmetic, the AES polynomial x^8 + x^4 + x^3 + x + 1 (0x11B) --
uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1u) p ^= a;
        const bool hi = a & 0x80u;
        a = static_cast<uint8_t>(a << 1);
        if (hi) a ^= 0x1Bu;
        b >>= 1;
    }
    return p;
}

uint8_t gf_inv(uint8_t a) {
    // The multiplicative group of GF(2^8) has order 255, so the inverse of a
    // is a^254. Binary powering; 0 maps to 0 by convention.
    uint8_t result = 1;
    uint8_t base = a;
    unsigned e = 254;
    while (e) {
        if (e & 1u) result = gf_mul(result, base);
        base = gf_mul(base, base);
        e >>= 1;
    }
    return result;
}

struct AesTables {
    uint8_t sbox[256];
    uint8_t inv_sbox[256];
    uint8_t rcon[11];  // rcon[1..10]; index 0 unused

    AesTables() {
        for (int a = 0; a < 256; a++) {
            // The affine transform of FIPS 197 section 5.1.1 over the inverse.
            const uint8_t inv = gf_inv(static_cast<uint8_t>(a));
            const uint8_t s = static_cast<uint8_t>(
                inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^
                rotl8(inv, 4) ^ 0x63u);
            sbox[a] = s;
        }
        for (int a = 0; a < 256; a++) inv_sbox[sbox[a]] = static_cast<uint8_t>(a);
        uint8_t r = 1;
        rcon[0] = 0;
        for (int i = 1; i < 11; i++) {
            rcon[i] = r;
            r = gf_mul(r, 2);  // xtime in this field
        }
    }

private:
    static uint8_t rotl8(uint8_t v, int n) {
        return static_cast<uint8_t>((v << n) | (v >> (8 - n)));
    }
};

const AesTables& tables() {
    static const AesTables t;
    return t;
}

// Round-key material: 60 words for AES-256 (Nr = 14). Words are FIPS 197
// words -- the first byte of the key is the *most* significant byte of w[0]
// -- and each word serializes big-endian into the 16-byte round key, because
// byte (24 - 8r) of word c is what row r of column c gets XORed with.
struct RoundKeys {
    uint8_t bytes[16 * 15];  // 15 round keys of 16 bytes

    explicit RoundKeys(const uint8_t key[Aes256Cbc::kKeyBytes]) {
        const AesTables& t = tables();
        uint32_t w[60];
        for (int i = 0; i < 8; i++) {
            w[i] = uint32_t(key[4 * i]) << 24 | uint32_t(key[4 * i + 1]) << 16 |
                   uint32_t(key[4 * i + 2]) << 8 | uint32_t(key[4 * i + 3]);
        }
        for (int i = 8; i < 60; i++) {
            uint32_t temp = w[i - 1];
            if (i % 8 == 0) {
                temp = sub_word(rot_word(temp)) ^ (uint32_t(t.rcon[i / 8]) << 24);
            } else if (i % 8 == 4) {
                temp = sub_word(temp);
            }
            w[i] = w[i - 8] ^ temp;
        }
        for (int round = 0; round < 15; round++) {
            for (int c = 0; c < 4; c++) {
                const uint32_t word = w[4 * round + c];
                bytes[16 * round + 4 * c + 0] = uint8_t(word >> 24);
                bytes[16 * round + 4 * c + 1] = uint8_t(word >> 16);
                bytes[16 * round + 4 * c + 2] = uint8_t(word >> 8);
                bytes[16 * round + 4 * c + 3] = uint8_t(word);
            }
        }
    }

private:
    static uint32_t sub_word(uint32_t x) {
        const AesTables& t = tables();
        return (uint32_t(t.sbox[(x >> 24) & 0xFF]) << 24) |
               (uint32_t(t.sbox[(x >> 16) & 0xFF]) << 16) |
               (uint32_t(t.sbox[(x >> 8) & 0xFF]) << 8) |
               uint32_t(t.sbox[x & 0xFF]);
    }
    static uint32_t rot_word(uint32_t x) { return (x << 8) | (x >> 24); }
};

void add_round_key(uint8_t st[16], const uint8_t rk[16]) {
    for (int i = 0; i < 16; i++) st[i] ^= rk[i];
}

void inv_shift_rows(uint8_t st[16]) {
    // State is column-major: st[r + 4c]. InvShiftRows rotates row r right by
    // r: new[c] = old[(c - r) mod 4]. Written as a table move, not a clever
    // in-place shuffle -- the clever version is how the bug got in.
    uint8_t row[4], out[4];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) row[c] = st[r + 4 * c];
        for (int c = 0; c < 4; c++) out[c] = row[(c - r) & 3];
        for (int c = 0; c < 4; c++) st[r + 4 * c] = out[c];
    }
}

void inv_sub_bytes(uint8_t st[16]) {
    const AesTables& t = tables();
    for (int i = 0; i < 16; i++) st[i] = t.inv_sbox[st[i]];
}

void inv_mix_column(uint8_t* col) {
    const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
    col[0] = gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9);
    col[1] = gf_mul(a0, 9)  ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13);
    col[2] = gf_mul(a0, 13) ^ gf_mul(a1, 9)  ^ gf_mul(a2, 14) ^ gf_mul(a3, 11);
    col[3] = gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9)  ^ gf_mul(a3, 14);
}

void decrypt_block(const RoundKeys& rk, const uint8_t in[16], uint8_t out[16]) {
    uint8_t st[16];
    for (int i = 0; i < 16; i++) st[i] = in[i];

    add_round_key(st, rk.bytes + 16 * 14);
    for (int round = 13; round >= 1; round--) {
        inv_shift_rows(st);
        inv_sub_bytes(st);
        add_round_key(st, rk.bytes + 16 * round);
        for (int c = 0; c < 4; c++) inv_mix_column(st + 4 * c);
    }
    inv_shift_rows(st);
    inv_sub_bytes(st);
    add_round_key(st, rk.bytes);

    for (int i = 0; i < 16; i++) out[i] = st[i];
}

}  // namespace

bool Aes256Cbc::decrypt(const uint8_t key[kKeyBytes], const uint8_t iv[kIvBytes],
                        const uint8_t* in, uint8_t* out, size_t len) {
    if (len % kBlock != 0) return false;
    const RoundKeys rk(key);
    uint8_t chain[kIvBytes];
    for (size_t i = 0; i < kIvBytes; i++) chain[i] = iv[i];
    for (size_t off = 0; off < len; off += kBlock) {
        uint8_t plain[kBlock];
        decrypt_block(rk, in + off, plain);
        for (size_t i = 0; i < kBlock; i++) out[off + i] = plain[i] ^ chain[i];
        for (size_t i = 0; i < kBlock; i++) chain[i] = in[off + i];
    }
    return true;
}

}  // namespace hakios
