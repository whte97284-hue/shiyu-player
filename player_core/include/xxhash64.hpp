#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kp {

// XXH64 (流式):比逐字节 FNV 快一个数量级,大文件哈希时瓶颈回到磁盘
class Xxh64 {
public:
    explicit Xxh64(uint64_t seed = 0) {
        v_[0] = seed + kP1 + kP2;
        v_[1] = seed + kP2;
        v_[2] = seed;
        v_[3] = seed - kP1;
        seed_ = seed;
    }
    void Add(const uint8_t* p, size_t n) {
        total_ += n;
        if (bufLen_ + n < 32) {
            std::memcpy(buf_ + bufLen_, p, n);
            bufLen_ += n;
            return;
        }
        if (bufLen_) {
            const size_t fill = 32 - bufLen_;
            std::memcpy(buf_ + bufLen_, p, fill);
            Stripe(buf_);
            p += fill;
            n -= fill;
            bufLen_ = 0;
        }
        while (n >= 32) {
            Stripe(p);
            p += 32;
            n -= 32;
        }
        std::memcpy(buf_, p, n);
        bufLen_ = n;
    }
    uint64_t Digest() const {
        uint64_t h;
        if (total_ >= 32) {
            h = Rotl(v_[0], 1) + Rotl(v_[1], 7) + Rotl(v_[2], 12) + Rotl(v_[3], 18);
            for (uint64_t v : v_) h = (h ^ Round(0, v)) * kP1 + kP4;
        } else {
            h = seed_ + kP5;
        }
        h += total_;
        const uint8_t* p = buf_;
        size_t n = bufLen_;
        while (n >= 8) {
            h ^= Round(0, Read64(p));
            h = Rotl(h, 27) * kP1 + kP4;
            p += 8;
            n -= 8;
        }
        if (n >= 4) {
            h ^= static_cast<uint64_t>(Read32(p)) * kP1;
            h = Rotl(h, 23) * kP2 + kP3;
            p += 4;
            n -= 4;
        }
        while (n--) {
            h ^= (*p++) * kP5;
            h = Rotl(h, 11) * kP1;
        }
        h ^= h >> 33;
        h *= kP2;
        h ^= h >> 29;
        h *= kP3;
        h ^= h >> 32;
        return h;
    }

private:
    static constexpr uint64_t kP1 = 11400714785074694791ull, kP2 = 14029467366897019727ull, kP3 = 1609587929392839161ull,
                              kP4 = 9650029242287828579ull, kP5 = 2870177450012600261ull;
    static uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
    static uint64_t Read64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }  // 小端平台
    static uint32_t Read32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
    static uint64_t Round(uint64_t acc, uint64_t in) { return Rotl(acc + in * kP2, 31) * kP1; }
    void Stripe(const uint8_t* p) {
        for (int i = 0; i < 4; ++i) v_[i] = Round(v_[i], Read64(p + i * 8));
    }
    uint64_t v_[4];
    uint64_t seed_ = 0, total_ = 0;
    uint8_t buf_[32];
    size_t bufLen_ = 0;
};

}  // namespace kp
