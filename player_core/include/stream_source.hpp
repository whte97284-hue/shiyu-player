#pragma once
#include <cstdint>
#include <memory>
#include <string>

// ============================================================================
//  StreamSource —— 在线流式播放的数据源
//
//  背景：AudioPlaybackEngine 原本只支持文件级解码（ma_decoder_init_file），
//  边下边读时 miniaudio 会在首次 EOF 就判定结束，所以必须换成「缓冲源」：
//  后台线程持续下载，把字节追加到从 0 开始连续的缓冲区；解码线程通过 ReadAt
//  取数据，数据没到就**等待**而不是返回 EOF。
//
//  约定（两个实现方必须一致）：
//    · 缓冲区是 append-only 的，offset 从 0 开始连续，没有空洞
//    · ReadAt 只有在 offset >= AvailableBytes() 且下载未结束时才等待
//    · 下载完成（Finished）之后，offset 超过末尾才返回 0 = 真 EOF
//    · 任何时刻都可以从多个线程调用 ReadAt（解码线程通常只有一个）
// ============================================================================

namespace stream_core {

class StreamSource {
public:
    virtual ~StreamSource() = default;

    // 从 offset 读最多 n 字节。返回实际读到的字节数。
    // 数据还没到且未结束 → 最多等 timeoutMs，超时返回 0（调用方应视为"暂时没数据"）。
    virtual size_t ReadAt(uint64_t offset, void* dst, size_t n, int timeoutMs) = 0;

    // 从 0 开始连续可用的字节数
    virtual uint64_t AvailableBytes() const = 0;

    // 总字节数（来自 Content-Length）；未知返回 0
    virtual uint64_t TotalBytes() const = 0;

    virtual bool Finished() const = 0;  // 下载正常结束
    virtual bool Failed() const = 0;    // 下载出错
    virtual std::string Error() const = 0;

    // 请求中止；之后 ReadAt 立即返回 0，后台线程尽快退出
    virtual void Cancel() = 0;
};

// 创建 HTTP 流式源并立即开始后台下载（http/https 均可）。
// 失败返回 nullptr 并填 error。url 必须是直接可下载的媒体地址。
std::shared_ptr<StreamSource> OpenHttpStream(const std::string& url,
                                             const std::string& userAgent,
                                             std::string* error);

} // namespace stream_core
