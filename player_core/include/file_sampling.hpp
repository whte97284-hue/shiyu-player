#pragma once
// 查重与整理共用:多线程小块读取、抽样内容指纹
#include "platform_io.hpp"
#include "xxhash64.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace kp {

// 并行读取文件的线程数:读文件头这类小块随机读,并行能明显缩短等待磁盘 (机械盘可以借 NCQ 重排寻道) 与杀毒软件的时间
inline unsigned IoThreads() {
    const unsigned hw = std::thread::hardware_concurrency();
    return std::max(2u, std::min(8u, hw ? hw : 4u));
}

// 在若干线程上并行处理 0..n-1;进度回调只在调用线程上触发
inline void ParallelFor(size_t n, unsigned threads, const std::function<void(size_t)>& fn, const std::function<void()>& tick,
                        const std::atomic<bool>* cancel) {
    if (n == 0) return;
    threads = std::max(1u, std::min<unsigned>(threads, static_cast<unsigned>(n)));
    std::atomic<size_t> next{0};
    auto worker = [&]() {
        for (size_t i; (i = next.fetch_add(1)) < n;) {
            if (cancel && cancel->load()) return;
            fn(i);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
    for (size_t i; (i = next.fetch_add(1)) < n;) {
        if (cancel && cancel->load()) break;
        fn(i);
        if (tick) tick();
    }
    for (auto& th : pool) th.join();
    if (tick) tick();
}

// 线程内复用的大块读写缓冲 (slot 0 / 1 各一块,两文件比对时各用一块):
// 复制、校验、比对每个文件都要 MB 级缓冲,反复申请释放既费时又容易把堆撑大。
// 后台任务跑在各自的线程上,线程结束时缓冲随之释放
inline std::vector<uint8_t>& ScratchBuffer(int slot, size_t size) {
    thread_local std::vector<uint8_t> bufs[2];
    std::vector<uint8_t>& b = bufs[slot & 1];
    if (b.size() < size) b.resize(size);
    return b;
}

// 整个文件的抽样指纹:头、尾和中间均匀 5 处,各 64KB (约 448KB),文件不大于这些时哈希全文。
// 大小相同、7 处内容都一样的不同音乐文件几乎不可能出现;只用于「不搬 / 标成重复」这类可回退的判断,
// 不可撤销的操作仍要逐字节确认
inline bool SampledFingerprint(const std::wstring& path, uint64_t& out) {
    platform::ReadOnlyFile f;
    if (!f.Open(path, true)) return false;
    const uint64_t size = f.Size(), q = 64 << 10;
    constexpr int kSamples = 7;
    Xxh64 h(size ^ 0x165667B19E3779F9ull);
    std::vector<uint8_t> buf(static_cast<size_t>(q));
    auto add = [&](uint64_t off, uint64_t len) {
        while (len > 0) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(len, buf.size()));
            const size_t n = f.ReadAt(off, buf.data(), want);
            if (n == 0) return false;
            h.Add(buf.data(), n);
            off += n;
            len -= n;
        }
        return true;
    };
    if (size <= kSamples * q) {
        if (!add(0, size)) return false;
    } else {
        for (int j = 0; j < kSamples; ++j)
            if (!add((size - q) * j / (kSamples - 1), q)) return false;
    }
    out = h.Digest();
    return true;
}

} // namespace kp
