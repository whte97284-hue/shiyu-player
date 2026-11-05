#include "kgg_decrypt.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "platform_io.hpp"

namespace kugou {
namespace {

// SQLite 全用大端
inline uint32_t ReadU32BE(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
// KGG 头是小端
inline uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// ===========================================================================
//  MD5
// ===========================================================================
struct Md5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t len = 0;
    uint8_t buf[64];
    size_t used = 0;

    static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t m[16];
        for (int i = 0; i < 16; ++i) m[i] = static_cast<uint32_t>(p[i * 4]) | (static_cast<uint32_t>(p[i * 4 + 1]) << 8) |
                                           (static_cast<uint32_t>(p[i * 4 + 2]) << 16) |
                                           (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) { f = (B & C) | (~B & D); g = i; }
            else if (i < 32) { f = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { f = C ^ (B | ~D); g = (7 * i) % 16; }
            const uint32_t tmp = D;
            D = C;
            C = B;
            B = B + rotl(A + f + K[i] + m[g], S[i]);
            A = tmp;
        }
        a += A; b += B; c += C; d += D;
    }

    void Update(const uint8_t* p, size_t n) {
        len += n;
        while (n > 0) {
            const size_t take = std::min(n, sizeof(buf) - used);
            std::memcpy(buf + used, p, take);
            used += take;
            p += take;
            n -= take;
            if (used == 64) { block(buf); used = 0; }
        }
    }

    void Final(uint8_t out[16]) {
        const uint64_t bits = len * 8;
        const uint8_t pad = 0x80;
        Update(&pad, 1);
        const uint8_t zero = 0;
        while (used != 56) Update(&zero, 1);
        uint8_t lenbuf[8];
        for (int i = 0; i < 8; ++i) lenbuf[i] = static_cast<uint8_t>(bits >> (8 * i));
        Update(lenbuf, 8);
        const uint32_t v[4] = {a, b, c, d};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(v[i] >> (8 * j));
    }
};

// ===========================================================================
//  AES-128 解密(CBC 用,自实现避免依赖 tag 模块内部类)
// ===========================================================================
class Aes128 {
public:
    explicit Aes128(const uint8_t key[16]) {
        std::memcpy(rk_, key, 16);
        uint8_t rcon = 1;
        for (int i = 16; i < 176; i += 4) {
            uint8_t t[4] = {rk_[i - 4], rk_[i - 3], rk_[i - 2], rk_[i - 1]};
            if (i % 16 == 0) {
                const uint8_t tmp = t[0];
                t[0] = static_cast<uint8_t>(Sbox[t[1]] ^ rcon);
                t[1] = Sbox[t[2]];
                t[2] = Sbox[t[3]];
                t[3] = Sbox[tmp];
                rcon = static_cast<uint8_t>((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
            }
            for (int k = 0; k < 4; ++k) rk_[i + k] = static_cast<uint8_t>(rk_[i - 16 + k] ^ t[k]);
        }
    }

    void DecryptBlock(const uint8_t in[16], uint8_t out[16]) const {
        uint8_t s[16];
        std::memcpy(s, in, 16);
        AddRoundKey(s, 10);
        for (int r = 9; r >= 1; --r) {
            InvShiftRows(s);
            InvSubBytes(s);
            AddRoundKey(s, r);
            InvMixColumns(s);
        }
        InvShiftRows(s);
        InvSubBytes(s);
        AddRoundKey(s, 0);
        std::memcpy(out, s, 16);
    }

private:
    static const uint8_t Sbox[256];
    static const uint8_t InvSbox[256];
    uint8_t rk_[176];

    static uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
    static uint8_t mul(uint8_t a, uint8_t b) {
        uint8_t r = 0;
        while (b) {
            if (b & 1) r ^= a;
            a = xtime(a);
            b >>= 1;
        }
        return r;
    }
    void AddRoundKey(uint8_t s[16], int round) const {
        for (int i = 0; i < 16; ++i) s[i] ^= rk_[round * 16 + i];
    }
    static void InvSubBytes(uint8_t s[16]) {
        for (int i = 0; i < 16; ++i) s[i] = InvSbox[s[i]];
    }
    static void InvShiftRows(uint8_t s[16]) {
        uint8_t t;
        // 第 1 行右移 1
        t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
        // 第 2 行右移 2
        t = s[2]; s[2] = s[10]; s[10] = t;
        t = s[6]; s[6] = s[14]; s[14] = t;
        // 第 3 行右移 3(= 左移 1)
        t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
    }
    static void InvMixColumns(uint8_t s[16]) {
        for (int c = 0; c < 4; ++c) {
            uint8_t* p = s + c * 4;
            const uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
            p[0] = static_cast<uint8_t>(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
            p[1] = static_cast<uint8_t>(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
            p[2] = static_cast<uint8_t>(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
            p[3] = static_cast<uint8_t>(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
        }
    }
};

const uint8_t Aes128::Sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

const uint8_t Aes128::InvSbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d};

// ===========================================================================
//  Base64
// ===========================================================================
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool Base64Decode(const std::string& in, std::string& out) {
    out.clear();
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        const char* p = std::strchr(kB64, c);
        if (!p || c == '\0') {
            if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
            return false;
        }
        val = (val << 6) + static_cast<int>(p - kB64);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return true;
}

// ===========================================================================
//  腾讯 TEA:16 轮、大端、sum 初值 delta*16
// ===========================================================================
constexpr uint32_t kDelta = 0x9E3779B9u;

uint32_t Bswap(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

void TeaDecryptBlock(uint32_t& v0, uint32_t& v1, const uint32_t k[4]) {
    uint32_t sum = kDelta * 16;
    for (int i = 0; i < 16; ++i) {
        v1 -= ((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]);
        v0 -= ((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]);
        sum -= kDelta;
    }
}

std::vector<uint8_t> TeaDecrypt(const uint8_t* data, size_t n, const uint8_t key[16]) {
    std::vector<uint8_t> out(n);
    uint32_t k[4];
    for (int i = 0; i < 4; ++i) {
        const uint8_t* p = key + i * 4;
        k[i] = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | p[3];
    }
    for (size_t off = 0; off + 8 <= n; off += 8) {
        const uint8_t* p = data + off;
        uint32_t v0 = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                      (static_cast<uint32_t>(p[2]) << 8) | p[3];
        uint32_t v1 = (static_cast<uint32_t>(p[4]) << 24) | (static_cast<uint32_t>(p[5]) << 16) |
                      (static_cast<uint32_t>(p[6]) << 8) | p[7];
        TeaDecryptBlock(v0, v1, k);
        out[off] = static_cast<uint8_t>(v0 >> 24);
        out[off + 1] = static_cast<uint8_t>(v0 >> 16);
        out[off + 2] = static_cast<uint8_t>(v0 >> 8);
        out[off + 3] = static_cast<uint8_t>(v0);
        out[off + 4] = static_cast<uint8_t>(v1 >> 24);
        out[off + 5] = static_cast<uint8_t>(v1 >> 16);
        out[off + 6] = static_cast<uint8_t>(v1 >> 8);
        out[off + 7] = static_cast<uint8_t>(v1);
    }
    return out;
}

uint8_t SimpleKeyByte(int i) {
    const double t = std::tan(106.0 + i * 0.1);
    return static_cast<uint8_t>(static_cast<int>(std::fabs(t) * 100.0) & 0xFF);
}

// decryptTencentTea: 非标准 CBC,首块带 pad/salt,尾部 7 字节零校验
bool DecryptTencentTea(const std::vector<uint8_t>& in, const uint8_t teaKey[16], std::vector<uint8_t>& out) {
    const size_t saltLen = 2, zeroLen = 7;
    if (in.size() % 8 != 0 || in.size() < 16) return false;

    std::vector<uint8_t> first = TeaDecrypt(in.data(), 8, teaKey);
    const int pad = first[0] & 0x7;
    const long long outLen = static_cast<long long>(in.size()) - 1 - pad - saltLen - zeroLen;
    if (outLen <= 0) return false;

    std::vector<uint8_t> dest(first);
    std::vector<uint8_t> ivPrev(8, 0), ivCur(in.begin(), in.begin() + 8);
    size_t pos = 8;
    size_t destIdx = static_cast<size_t>(1 + pad);
    out.assign(static_cast<size_t>(outLen), 0);

    auto cryptBlock = [&]() {
        ivPrev = ivCur;
        ivCur.assign(in.begin() + pos, in.begin() + pos + 8);
        for (int i = 0; i < 8; ++i) dest[i] ^= in[pos + i];
        dest = TeaDecrypt(dest.data(), 8, teaKey);
        pos += 8;
        destIdx = 0;
    };

    for (size_t i = 1; i <= saltLen;) {
        if (destIdx < 8) { ++destIdx; ++i; }
        else if (destIdx == 8) cryptBlock();
    }

    size_t outPos = 0;
    while (outPos < static_cast<size_t>(outLen)) {
        if (destIdx < 8) {
            out[outPos] = static_cast<uint8_t>(dest[destIdx] ^ ivPrev[destIdx]);
            ++destIdx;
            ++outPos;
        } else if (destIdx == 8) {
            cryptBlock();
        }
    }
    for (size_t i = 0; i < zeroLen; ++i)
        if (dest[destIdx] != ivPrev[destIdx]) return false;
    return true;
}

// ekey(字符串) -> QMC 密钥
bool DeriveKey(const std::string& ekey, std::vector<uint8_t>& key, std::string& err) {
    std::string raw;
    if (!Base64Decode(ekey, raw) || raw.size() < 16) {
        err = "ekey base64 解码失败";
        return false;
    }
    uint8_t teaKey[16];
    for (int i = 0; i < 8; ++i) {
        teaKey[i * 2] = SimpleKeyByte(i);
        teaKey[i * 2 + 1] = static_cast<uint8_t>(raw[i]);
    }
    std::vector<uint8_t> body(raw.begin() + 8, raw.end());
    std::vector<uint8_t> rest;
    if (!DecryptTencentTea(body, teaKey, rest)) {
        err = "ekey TEA 解密失败";
        return false;
    }
    key.assign(raw.begin(), raw.begin() + 8);
    key.insert(key.end(), rest.begin(), rest.end());
    return true;
}

// ===========================================================================
//  QMC RC4 流密码
// ===========================================================================
class QmcRc4 {
public:
    explicit QmcRc4(const std::vector<uint8_t>& key) : key_(key), n_(static_cast<int>(key.size())) {
        box_.resize(n_);
        for (int i = 0; i < n_; ++i) box_[i] = static_cast<uint8_t>(i);
        int j = 0;
        for (int i = 0; i < n_; ++i) {
            j = (j + box_[i] + key_[i % n_]) % n_;
            std::swap(box_[i], box_[j]);
        }
        hash_ = 1;
        for (int i = 0; i < n_; ++i) {
            const uint32_t v = key_[i];
            if (v == 0) continue;
            const uint32_t nxt = hash_ * v;
            if (nxt == 0 || nxt <= hash_) break;
            hash_ = nxt;
        }
    }

    void Decrypt(uint8_t* buf, size_t len, size_t offset) const {
        constexpr size_t kSeg = 5120, kFirst = 128;
        size_t done = 0;
        while (done < len) {
            const size_t cur = offset + done;
            if (cur < kFirst) {
                const size_t n = std::min(len - done, kFirst - cur);
                for (size_t i = 0; i < n; ++i) buf[done + i] ^= key_[Skip(cur + i)];
                done += n;
                continue;
            }
            if (cur % kSeg != 0) {
                const size_t n = std::min(len - done, kSeg - cur % kSeg);
                Seg(buf + done, n, cur);
                done += n;
                continue;
            }
            const size_t n = std::min(len - done, kSeg);
            Seg(buf + done, n, cur);
            done += n;
        }
    }

private:
    std::vector<uint8_t> key_;
    std::vector<uint8_t> box_;
    int n_ = 0;
    uint32_t hash_ = 1;

    size_t Skip(size_t id) const {
        const int seed = key_[id % n_];
        if (seed == 0) return 0;
        const int64_t idx = static_cast<int64_t>(static_cast<double>(hash_) / ((id + 1) * seed) * 100.0);
        return static_cast<size_t>(idx % static_cast<int64_t>(n_));
    }

    void Seg(uint8_t* buf, size_t len, size_t offset) const {
        std::vector<uint8_t> box(box_);
        int j = 0, k = 0;
        const size_t skipLen = (offset % 5120) + Skip(offset / 5120);
        for (long long i = -static_cast<long long>(skipLen); i < static_cast<long long>(len); ++i) {
            j = (j + 1) % n_;
            k = (box[j] + k) % n_;
            std::swap(box[j], box[k]);
            if (i >= 0) buf[i] ^= box[(box[j] + box[k]) % n_];
        }
    }
};

// ===========================================================================
//  v3(老 KGM / KGMA):slot 表 + 倒序 MD5,逐字节异或
// ===========================================================================
// 酷狗把 MD5 摘要按 2 字节一组倒着取,不是直接输出
void KugouMd5(const uint8_t* data, size_t n, uint8_t out[16]) {
    Md5 m;
    if (n) m.Update(data, n);
    uint8_t d[16];
    m.Final(d);
    for (int i = 0; i < 16; i += 2) {
        out[i] = d[14 - i];
        out[i + 1] = d[15 - i];
    }
}

class KgmV3 {
public:
    bool Init(uint32_t slot, const uint8_t cryptoKey[16]) {
        static const uint8_t kSlot1[4] = {0x6C, 0x2C, 0x2F, 0x27};
        if (slot != 1) return false;
        KugouMd5(kSlot1, 4, slotBox_);
        uint8_t fb[16];
        KugouMd5(cryptoKey, 16, fb);
        std::memcpy(fileBox_, fb, 16);
        fileBox_[16] = 0x6B;
        return true;
    }

    void Decrypt(uint8_t* buf, size_t len, size_t offset) const {
        for (size_t i = 0; i < len; ++i) {
            const size_t pos = offset + i;
            uint8_t v = buf[i];
            v ^= fileBox_[pos % 17];
            v = static_cast<uint8_t>(v ^ (v << 4));
            v ^= slotBox_[pos % 16];
            const uint32_t x = static_cast<uint32_t>(pos);
            v ^= static_cast<uint8_t>(x ^ (x >> 8) ^ (x >> 16) ^ (x >> 24));
            buf[i] = v;
        }
    }

private:
    uint8_t slotBox_[16] = {};
    uint8_t fileBox_[17] = {};
};

// ===========================================================================
//  最小 SQLite 读取:只为从 KGMusicV3.db 取 ShareFileItems 的 (hash, ekey)
// ===========================================================================
class MiniSqlite {
public:
    bool Load(const std::vector<uint8_t>& buf) {
        buf_ = buf;
        if (buf_.size() < 100) return false;
        ps_ = (static_cast<int>(buf_[16]) << 8) | buf_[17];
        if (ps_ == 1) ps_ = 65536;
        if (ps_ < 512 || ps_ > 65536 || buf_.size() % ps_ != 0) return false;
        pages_ = buf_.size() / ps_;
        WalkMaster();
        return true;
    }

    // 遍历某张表的全部行,每行是原始字符串列表(只保留 TEXT)
    std::vector<std::vector<std::string>> Rows(const std::string& table) const {
        std::vector<std::vector<std::string>> out;
        auto it = tables_.find(table);
        if (it == tables_.end()) return out;
        std::vector<uint32_t> stack{it->second};
        while (!stack.empty()) {
            const uint32_t pn = stack.back();
            stack.pop_back();
            const uint8_t* p = Page(pn);
            if (!p) continue;
            const int hdr = (pn == 1) ? 100 : 0;
            const int typ = p[hdr];
            const int nc = (p[hdr + 3] << 8) | p[hdr + 4];
            const int ptrs = hdr + ((typ == 5) ? 12 : 8);
            if (typ == 5) {
                for (int i = 0; i < nc; ++i) {
                    const int cp = (p[ptrs + i * 2] << 8) | p[ptrs + i * 2 + 1];
                    if (cp + 4 > ps_) continue;
                    stack.push_back(ReadU32BE(p + cp));
                }
                const uint32_t right = ReadU32BE(p + hdr + 8);
                if (right) stack.push_back(right);
                continue;
            }
            for (int i = 0; i < nc; ++i) {
                const int cp = (p[ptrs + i * 2] << 8) | p[ptrs + i * 2 + 1];
                if (cp >= ps_) continue;
                size_t o = cp;
                const uint64_t plen = Varint(p, o, ps_);
                Varint(p, o, ps_); // rowid
                std::vector<uint8_t> payload;
                if (!Payload(p, o, plen, payload)) continue;
                std::vector<std::string> vals;
                ParseRecord(payload, vals);
                out.push_back(std::move(vals));
            }
        }
        return out;
    }

private:
    std::vector<uint8_t> buf_;
    int ps_ = 0;
    int pages_ = 0;
    std::map<std::string, uint32_t> tables_;

    const uint8_t* Page(uint32_t n) const {
        if (n == 0 || n > static_cast<uint32_t>(pages_)) return nullptr;
        return buf_.data() + (n - 1) * static_cast<size_t>(ps_);
    }
    static uint64_t Varint(const uint8_t* p, size_t& o, int limit) {
        uint64_t v = 0;
        for (int i = 0; i < 9; ++i) {
            if (static_cast<int>(o) >= limit) return v;
            const uint8_t c = p[o++];
            if (i == 8) return (v << 8) | c;
            v = (v << 7) | (c & 0x7F);
            if (!(c & 0x80)) return v;
        }
        return v;
    }
    // 处理溢出页
    bool Payload(const uint8_t* page, size_t off, uint64_t plen, std::vector<uint8_t>& out) const {
        const int usable = ps_;
        const int maxLocal = usable - 35;
        const int minLocal = (usable - 12) * 32 / 255 - 23;
        out.clear();
        if (static_cast<int>(plen) <= maxLocal) {
            if (off + plen > static_cast<size_t>(usable)) return false;
            out.assign(page + off, page + off + plen);
            return true;
        }
        int local = minLocal + static_cast<int>((plen - minLocal) % (usable - 4));
        if (local > maxLocal) local = minLocal;
        out.assign(page + off, page + off + local);
        uint32_t nxt = ReadU32BE(page + off + local);
        int guard = 0;
        while (nxt && out.size() < plen && guard++ < 10000) {
            const uint8_t* p = Page(nxt);
            if (!p) return false;
            const size_t take = std::min<size_t>(usable - 4, static_cast<size_t>(plen) - out.size());
            out.insert(out.end(), p + 4, p + 4 + take);
            nxt = ReadU32BE(p);
        }
        return true;
    }
    static void ParseRecord(const std::vector<uint8_t>& payload, std::vector<std::string>& vals) {
        size_t o = 0;
        const uint64_t hlen = Varint(payload.data(), o, static_cast<int>(payload.size()));
        std::vector<uint64_t> types;
        size_t x = o;
        while (x < hlen && x < payload.size()) types.push_back(Varint(payload.data(), x, static_cast<int>(payload.size())));
        size_t y = static_cast<size_t>(hlen);
        for (uint64_t t : types) {
            if (t == 0) { vals.emplace_back(); continue; }
            if (t >= 1 && t <= 6) {
                static const int kSize[7] = {0, 1, 2, 3, 4, 6, 8};
                y += kSize[t];
                vals.emplace_back();
            } else if (t == 7) {
                y += 8;
                vals.emplace_back();
            } else if (t == 8 || t == 9) {
                vals.emplace_back();
            } else if (t >= 12) {
                const size_t n = static_cast<size_t>((t - 12) / 2);
                if (y + n <= payload.size() && (t % 2) == 1)
                    vals.emplace_back(reinterpret_cast<const char*>(payload.data() + y), n);
                else
                    vals.emplace_back();
                y += n;
            } else {
                vals.emplace_back();
            }
        }
    }
    void WalkMaster() {
        std::vector<uint32_t> stack{1};
        while (!stack.empty()) {
            const uint32_t pn = stack.back();
            stack.pop_back();
            const uint8_t* p = Page(pn);
            if (!p) continue;
            const int hdr = (pn == 1) ? 100 : 0;
            const int typ = p[hdr];
            const int nc = (p[hdr + 3] << 8) | p[hdr + 4];
            const int ptrs = hdr + ((typ == 5) ? 12 : 8);
            if (typ == 5) {
                for (int i = 0; i < nc; ++i) {
                    const int cp = (p[ptrs + i * 2] << 8) | p[ptrs + i * 2 + 1];
                    if (cp + 4 > ps_) continue;
                    stack.push_back(ReadU32BE(p + cp));
                }
                const uint32_t right = ReadU32BE(p + hdr + 8);
                if (right) stack.push_back(right);
                continue;
            }
            for (int i = 0; i < nc; ++i) {
                const int cp = (p[ptrs + i * 2] << 8) | p[ptrs + i * 2 + 1];
                if (cp >= ps_) continue;
                size_t o = cp;
                const uint64_t plen = Varint(p, o, ps_);
                Varint(p, o, ps_);
                std::vector<uint8_t> payload;
                if (!Payload(p, o, plen, payload)) continue;
                std::vector<std::string> vals;
                ParseRecord(payload, vals);
                if (vals.size() >= 4 && vals[0] == "table") {
                    // sqlite_master: type, name, tbl_name, rootpage, sql
                    if (vals.size() >= 4) {
                        // rootpage 是数值列,ParseRecord 里被丢掉了;这里从原始 payload 重取
                        // 简化:用第 4 个字段的位置推算不可靠,改为按类型重新解析一次
                        uint64_t root = 0;
                        if (ParseRootPage(payload, root)) tables_[vals[1]] = static_cast<uint32_t>(root);
                    }
                }
            }
        }
    }
    // 从 sqlite_master 的记录里取第 4 列(rootpage)的整数值
    static bool ParseRootPage(const std::vector<uint8_t>& payload, uint64_t& root) {
        size_t o = 0;
        const uint64_t hlen = Varint(payload.data(), o, static_cast<int>(payload.size()));
        std::vector<uint64_t> types;
        size_t x = o;
        while (x < hlen && x < payload.size()) types.push_back(Varint(payload.data(), x, static_cast<int>(payload.size())));
        size_t y = static_cast<size_t>(hlen);
        for (size_t i = 0; i < types.size(); ++i) {
            const uint64_t t = types[i];
            int size = 0;
            if (t >= 1 && t <= 6) {
                static const int kSize[7] = {0, 1, 2, 3, 4, 6, 8};
                size = kSize[t];
            } else if (t == 7) {
                size = 8;
            } else if (t >= 12) {
                size = static_cast<int>((t - 12) / 2);
            }
            if (i == 3 && size > 0 && size <= 8 && y + size <= payload.size()) {
                root = 0;
                for (int k = 0; k < size; ++k) root = (root << 8) | payload[y + k];
                return true;
            }
            y += static_cast<size_t>(size);
        }
        return false;
    }
};

// ===========================================================================
//  密钥库:解密 + 取 ekey
// ===========================================================================
const uint8_t kDbMaster[24] = {0x1D, 0x61, 0x31, 0x45, 0xB2, 0x47, 0xBF, 0x7F, 0x3D, 0x18, 0x96, 0x72,
                               0x14, 0x4F, 0xE4, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x73, 0x41, 0x6C, 0x54};

uint32_t NextPageIv(uint32_t seed) {
    const uint32_t left = seed * 0x9EF4u;
    const uint32_t right = (seed / 0xCE26u) * 0x7FFFFF07u;
    uint32_t v = left - right;
    if (v & 0x80000000u) v += 0x7FFFFF07u;
    return v;
}

void DerivePageKeyIv(uint32_t pageNo, uint8_t key[16], uint8_t iv[16]) {
    uint8_t mk[24];
    std::memcpy(mk, kDbMaster, 24);
    mk[0x10] = static_cast<uint8_t>(pageNo);
    mk[0x11] = static_cast<uint8_t>(pageNo >> 8);
    mk[0x12] = static_cast<uint8_t>(pageNo >> 16);
    mk[0x13] = static_cast<uint8_t>(pageNo >> 24);
    Md5 m1;
    m1.Update(mk, 24);
    m1.Final(key);

    uint8_t ivbuf[16] = {};
    uint32_t s = pageNo + 1;
    for (int i = 0; i < 16; i += 4) {
        s = NextPageIv(s);
        ivbuf[i] = static_cast<uint8_t>(s);
        ivbuf[i + 1] = static_cast<uint8_t>(s >> 8);
        ivbuf[i + 2] = static_cast<uint8_t>(s >> 16);
        ivbuf[i + 3] = static_cast<uint8_t>(s >> 24);
    }
    Md5 m2;
    m2.Update(ivbuf, 16);
    m2.Final(iv);
}

bool DecryptDbBuffer(std::vector<uint8_t>& buf) {
    constexpr size_t kPage = 0x400;
    if (buf.size() < kPage || buf.size() % kPage != 0) return false;
    if (std::memcmp(buf.data(), "SQLite format 3\0", 16) == 0) return true; // 未加密

    uint8_t key[16], iv[16];
    // page 1:头 0x10 字节的密文搬到 0x10,再解 0x10 之后
    uint8_t* p1 = buf.data();
    // 页 1 头的这两个字段是小端(不是 SQLite 的大端)
    const uint32_t o14 = static_cast<uint32_t>(p1[0x14]) | (static_cast<uint32_t>(p1[0x15]) << 8) |
                         (static_cast<uint32_t>(p1[0x16]) << 16) | (static_cast<uint32_t>(p1[0x17]) << 24);
    if (o14 != 0x20204000u) return false;
    uint8_t expected[8];
    std::memcpy(expected, p1 + 0x10, 8);
    std::memcpy(p1 + 0x10, p1 + 0x08, 8);

    DerivePageKeyIv(1, key, iv);
    Aes128 aes(key);
    uint8_t prev[16];
    std::memcpy(prev, iv, 16);
    for (size_t off = 0x10; off + 16 <= kPage; off += 16) {
        uint8_t cur[16];
        std::memcpy(cur, p1 + off, 16);
        uint8_t out[16];
        aes.DecryptBlock(cur, out);
        for (int i = 0; i < 16; ++i) p1[off + i] = static_cast<uint8_t>(out[i] ^ prev[i]);
        std::memcpy(prev, cur, 16);
    }
    if (std::memcmp(p1 + 0x10, expected, 8) != 0) return false;
    std::memcpy(p1, "SQLite format 3\0", 16);

    const size_t pages = buf.size() / kPage;
    for (size_t pg = 2; pg <= pages; ++pg) {
        DerivePageKeyIv(static_cast<uint32_t>(pg), key, iv);
        Aes128 aes2(key);
        uint8_t* p = buf.data() + (pg - 1) * kPage;
        std::memcpy(prev, iv, 16);
        for (size_t off = 0; off + 16 <= kPage; off += 16) {
            uint8_t cur[16];
            std::memcpy(cur, p + off, 16);
            uint8_t out[16];
            aes2.DecryptBlock(cur, out);
            for (int i = 0; i < 16; ++i) p[off + i] = static_cast<uint8_t>(out[i] ^ prev[i]);
            std::memcpy(prev, cur, 16);
        }
    }
    return true;
}

struct EkeyTable {
    std::unordered_map<std::string, std::string> map;
    bool loaded = false;
    std::mutex mtx;

    bool Load(const std::wstring& dbPath) {
        std::lock_guard<std::mutex> lk(mtx);
        if (loaded) return true;
        platform::ReadOnlyFile f;
        if (!f.Open(dbPath, true) || f.Size() == 0 || f.Size() > (256u << 20)) return false;
        std::vector<uint8_t> buf(static_cast<size_t>(f.Size()));
        if (f.ReadAt(0, buf.data(), buf.size()) != buf.size()) return false;
        if (!DecryptDbBuffer(buf)) return false;
        MiniSqlite db;
        if (!db.Load(buf)) return false;
        auto rows = db.Rows("ShareFileItems");
        for (auto& r : rows) {
            if (r.size() <= 24) continue;
            const std::string& h = r[23];
            const std::string& k = r[24];
            if (h.size() == 32 && k.size() > 200) map[h] = k;
        }
        loaded = true;
        return true;
    }
};

EkeyTable& Ekeys() {
    static EkeyTable t;
    return t;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string SniffExt(const uint8_t* h) {
    if (std::memcmp(h, "fLaC", 4) == 0) return ".flac";
    if (std::memcmp(h, "OggS", 4) == 0) return ".ogg";
    if (std::memcmp(h, "ID3", 3) == 0) return ".mp3";
    if (std::memcmp(h + 4, "ftyp", 4) == 0) return ".m4a";
    if (std::memcmp(h, "RIFF", 4) == 0) return ".wav";
    return {};
}

} // namespace

// ===========================================================================
//  对外接口
// ===========================================================================
std::wstring KggDecryptor::FindDatabase() {
    std::vector<std::wstring> cands;
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
        std::wstring base(buf, n);
        cands.push_back(base + L"\\KuGou8\\KGMusicV3.db");
        cands.push_back(base + L"\\KuGou\\KGMusicV3.db");
    }
    const DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    if (m > 0 && m < MAX_PATH * 2) {
        std::wstring base(buf, m);
        cands.push_back(base + L"\\KuGou\\KGMusicV3.db");
        cands.push_back(base + L"\\KuGou8\\KGMusicV3.db");
    }
    for (const auto& c : cands)
        if (platform::PathExists(c)) return c;
    return {};
}

bool KggDecryptor::Handles(const std::wstring& path) {
    std::wstring ext;
    const size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext == L".kgg" || ext == L".kgm" || ext == L".kgma" || ext == L".vpr";
}

KggResult KggDecryptor::Decrypt(const std::wstring& src, const std::wstring& outDir, const std::wstring& dbPath) {
    KggResult r;
    platform::ReadOnlyFile f;
    if (!f.Open(src, true) || f.Size() < 0x400) {
        r.code = "io";
        r.detail = "无法读取源文件";
        return r;
    }
    std::vector<uint8_t> head(0x400);
    if (f.ReadAt(0, head.data(), head.size()) != head.size()) {
        r.code = "io";
        r.detail = "读取文件头失败";
        return r;
    }
    static const uint8_t kKgmMagic[16] = {0x7C, 0xD5, 0x32, 0xEB, 0x86, 0x02, 0x7F, 0x4B,
                                          0xA8, 0xAF, 0xA6, 0x8E, 0x0F, 0xFF, 0x99, 0x14};
    static const uint8_t kVprMagic[16] = {0x05, 0x28, 0xBC, 0x96, 0xE9, 0xE4, 0x5A, 0x43,
                                          0x91, 0xAA, 0xBD, 0xD0, 0x7A, 0xF5, 0x36, 0x31};
    const bool magicOk = std::memcmp(head.data(), kKgmMagic, 16) == 0 || std::memcmp(head.data(), kVprMagic, 16) == 0;
    if (!magicOk) {
        r.code = "bad-header";
        r.detail = "不是 KGM/VPR 容器";
        return r;
    }
    const uint32_t audioOff = ReadU32LE(head.data() + 0x10);
    const uint32_t cryptoVer = ReadU32LE(head.data() + 0x14);
    if (audioOff >= f.Size()) {
        r.code = "bad-header";
        r.detail = "音频偏移越界";
        return r;
    }
    if (cryptoVer != 5 && cryptoVer != 3) {
        r.code = "unsupported-version";
        r.detail = "crypto version " + std::to_string(cryptoVer) + "(仅支持 v3/v5)";
        return r;
    }

    // v3(老 KGM/KGMA):不用 ekey,用文件头里的 slot 表和 CryptoKey
    if (cryptoVer == 3) {
        const uint32_t slot = ReadU32LE(head.data() + 0x18);
        KgmV3 cipher;
        if (!cipher.Init(slot, head.data() + 0x2c)) {
            r.code = "unsupported-version";
            r.detail = "未知的 crypto slot " + std::to_string(slot);
            return r;
        }
        const size_t audioLen = static_cast<size_t>(f.Size() - audioOff);
        std::vector<uint8_t> data(audioLen);
        if (f.ReadAt(audioOff, data.data(), audioLen) != audioLen) {
            r.code = "io";
            r.detail = "读取音频数据失败";
            return r;
        }
        cipher.Decrypt(data.data(), data.size(), 0);
        const std::string ext3 = SniffExt(data.data());
        if (ext3.empty()) {
            r.code = "decode";
            r.detail = "解密后不是已知音频格式";
            return r;
        }
        std::filesystem::path out(outDir.empty() ? std::filesystem::path(src)
                                                 : std::filesystem::path(outDir) / std::filesystem::path(src).filename());
        out.replace_extension(ToWide(ext3));
        platform::WritableFile wf;
        if (!wf.Create(out.wstring())) {
            r.code = "io";
            r.detail = "写入失败";
            return r;
        }
        wf.Write(data.data(), data.size());
        wf.Close();
        r.ok = true;
        r.output = out.wstring();
        return r;
    }

    // v5:靠 audio hash 去密钥库查 ekey
    const uint32_t hashLen = ReadU32LE(head.data() + 0x44);
    if (hashLen == 0 || hashLen > 128 || 0x48 + hashLen > head.size()) {
        r.code = "bad-header";
        r.detail = "audio hash 长度异常";
        return r;
    }
    const std::string hash(reinterpret_cast<const char*>(head.data() + 0x48), hashLen);

    const std::wstring db = dbPath.empty() ? FindDatabase() : dbPath;
    if (db.empty()) {
        r.code = "db";
        r.detail = "找不到 KGMusicV3.db";
        return r;
    }
    EkeyTable& tbl = Ekeys();
    if (!tbl.Load(db)) {
        r.code = "db";
        r.detail = "密钥库解密失败";
        return r;
    }
    std::string ekey;
    {
        std::lock_guard<std::mutex> lk(tbl.mtx);
        auto it = tbl.map.find(hash);
        if (it != tbl.map.end()) ekey = it->second;
    }
    if (ekey.empty()) {
        r.code = "no-ekey";
        r.detail = "库里没有这首的密钥 —— 在酷狗里播放一次即可";
        return r;
    }

    std::vector<uint8_t> key;
    std::string err;
    if (!DeriveKey(ekey, key, err)) {
        r.code = "decode";
        r.detail = err;
        return r;
    }
    if (key.size() > 300) {
        // RC4 分支
        const size_t audioLen = static_cast<size_t>(f.Size() - audioOff);
        std::vector<uint8_t> data(audioLen);
        if (f.ReadAt(audioOff, data.data(), audioLen) != audioLen) {
            r.code = "io";
            r.detail = "读取音频数据失败";
            return r;
        }
        QmcRc4 cipher(key);
        cipher.Decrypt(data.data(), data.size(), 0);
        const std::string ext = SniffExt(data.data());
        if (ext.empty()) {
            r.code = "decode";
            r.detail = "解密后不是已知音频格式";
            return r;
        }
        std::filesystem::path out(outDir.empty() ? std::filesystem::path(src) : std::filesystem::path(outDir) /
                                                                                  std::filesystem::path(src).filename());
        out.replace_extension(ToWide(ext));
        platform::WritableFile wf;
        if (!wf.Create(out.wstring())) {
            r.code = "io";
            r.detail = "写入失败";
            return r;
        }
        wf.Write(data.data(), data.size());
        wf.Close();
        r.ok = true;
        r.output = out.wstring();
        return r;
    }

    r.code = "decode";
    r.detail = "短密钥(map)分支尚未接入";
    return r;
}

std::vector<KggResult> KggDecryptor::DecryptBatch(const std::vector<std::wstring>& files, const std::wstring& outDir,
                                                  int threads) {
    std::vector<KggResult> out(files.size());
    if (files.empty()) return out;
    int n = threads;
    if (n <= 0) n = static_cast<int>(std::thread::hardware_concurrency());
    n = std::max(1, std::min(n, 16));

    std::atomic<size_t> next{0};
    auto worker = [&]() {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= files.size()) break;
            out[i] = Decrypt(files[i], outDir);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    return out;
}

} // namespace kugou
