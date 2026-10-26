#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace lyric_core {

// 轻量级自包含 ZLIB (RFC 1950) / DEFLATE (RFC 1951) 解压器，零外部第三方依赖
class ZlibDecompressor {
public:
    // 解压 ZLIB 包装流 (带 2 字节头与 4 字节 Adler32 校验)
    static bool DecompressZlib(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& dst);

    // 解压纯 DEFLATE 流
    static bool DecompressDeflate(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& dst);
};

} // namespace lyric_core
