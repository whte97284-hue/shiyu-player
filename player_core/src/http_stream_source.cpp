// HTTP 流式数据源:后台线程边下边播,解码线程按 offset 取数据
// 只依赖 Windows API (WinHTTP),player_core 不引入任何第三方库
//
// 为什么要自己攒缓冲区:流式解码器一旦读到 EOF 就认定「文件结束」,在线播放时
// 网络慢一点就会被判成播完。这里把「暂时没数据」和「真的结束」分开——前者让
// ReadAt 在条件变量上等,只有下载线程收尾了才返回 0。
#include "stream_source.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

namespace stream_core {
namespace {

// 64KB 是 WinHTTP 单次读的甜点:再小则 syscall 太密,再大对首播延迟没帮助
constexpr DWORD kReadChunk = 64 * 1024;
// 连接阶段卡住时靠这个超时兜底,Cancel 只能打断请求句柄,打不断正在进行的 connect
constexpr int kConnectTimeoutMs = 8000;
// 读超时故意设短:它同时充当「检查取消标志」的节拍
constexpr int kReadTimeoutMs = 3000;
// 连续读超时超过这个次数才认账(网络抖一下不该直接判死)
constexpr int kMaxIdleTimeouts = 10;

const char kDefaultUa[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) KugouPlayer/1.0";

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

struct ParsedUrl {
    bool secure = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring object; // 路径 + 查询串,不含 scheme/host
};

// WinHTTP 要的是拆开的四元组,而外面只给一个 URL 字符串
bool ParseUrl(const std::string& url, ParsedUrl& out, std::string* error) {
    const std::wstring w = Utf8ToWide(url);
    const size_t sep = w.find(L"://");
    if (sep == std::wstring::npos) {
        if (error) *error = "地址缺少 http:// 或 https:// 前缀";
        return false;
    }
    std::wstring scheme = w.substr(0, sep);
    for (wchar_t& c : scheme) c = static_cast<wchar_t>(::towlower(c));
    if (scheme != L"http" && scheme != L"https") {
        if (error) *error = "只支持 http/https 地址";
        return false;
    }
    out.secure = (scheme == L"https");

    const std::wstring rest = w.substr(sep + 3);
    // 路径与查询串从第一个 '/' 或 '?' 开始;没有就当根路径
    size_t cut = rest.find_first_of(L"/?");
    const std::wstring hostport = (cut == std::wstring::npos) ? rest : rest.substr(0, cut);
    out.object = (cut == std::wstring::npos) ? L"/" : rest.substr(cut);
    if (!out.object.empty() && out.object[0] == L'?') out.object = L"/" + out.object;

    if (hostport.empty()) {
        if (error) *error = "地址里没有主机名";
        return false;
    }
    // IPv6 字面量用 [::1]:port 形式,冒号在方括号里,不能按最后一个冒号切
    size_t colon = std::wstring::npos;
    if (hostport[0] == L'[') {
        const size_t close = hostport.find(L']');
        if (close != std::wstring::npos && close + 1 < hostport.size() && hostport[close + 1] == L':')
            colon = close + 1;
    } else {
        colon = hostport.rfind(L':');
    }
    if (colon != std::wstring::npos) {
        out.host = hostport.substr(0, colon);
        out.port = static_cast<INTERNET_PORT>(::_wtoi(hostport.c_str() + colon + 1));
    } else {
        out.host = hostport;
    }
    if (out.host.empty()) {
        if (error) *error = "地址里没有主机名";
        return false;
    }
    if (out.port == 0)
        out.port = out.secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    return true;
}

// 句柄的保底释放;请求句柄另有跨线程归属问题,不交给它
struct HandleCloser {
    HINTERNET h = nullptr;
    ~HandleCloser() {
        if (h) ::WinHttpCloseHandle(h);
    }
};

class HttpStreamSource final : public StreamSource {
public:
    HttpStreamSource(ParsedUrl url, std::wstring ua) : url_(std::move(url)), ua_(std::move(ua)) {}

    ~HttpStreamSource() override {
        // 先 Cancel 再 join:线程可能正卡在阻塞读里,不打断就得等满读超时
        Cancel();
        if (worker_.joinable()) worker_.join();
    }

    void Start() { worker_ = std::thread(&HttpStreamSource::DownloadLoop, this); }

    size_t ReadAt(uint64_t offset, void* dst, size_t n, int timeoutMs) override {
        if (dst == nullptr || n == 0) return 0;
        std::unique_lock<std::mutex> lk(mu_);
        if (canceled_) return 0;

        if (offset >= AvailableLocked()) {
            // 已经收尾了就别等:这里返回 0 对调用方意味着真 EOF
            if (finished_ || failed_) return 0;
            cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0),
                         [&] { return canceled_ || finished_ || failed_ || offset < AvailableLocked(); });
            if (canceled_) return 0;
            if (offset >= AvailableLocked()) return 0; // 超时,只是暂时没数据
        }
        const uint64_t avail = AvailableLocked() - offset;
        const size_t take = n < avail ? n : static_cast<size_t>(avail);
        std::memcpy(dst, buf_.data() + static_cast<size_t>(offset), take);
        return take;
    }

    uint64_t AvailableBytes() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return AvailableLocked();
    }

    uint64_t TotalBytes() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return total_;
    }

    bool Finished() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return finished_;
    }

    bool Failed() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return failed_;
    }

    std::string Error() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return error_;
    }

    void Cancel() override {
        HINTERNET req = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            canceled_ = true;
            // 把句柄从线程手里收走:谁拿到谁负责关闭,避免 double-close
            req = req_;
            req_ = nullptr;
        }
        cv_.notify_all();
        // 关闭正在使用的句柄是为了打断阻塞中的 WinHttpReadData;线程那边读失败后
        // 会先看 canceled_,不会把它记成错误
        if (req) ::WinHttpCloseHandle(req);
    }

private:
    uint64_t AvailableLocked() const { return static_cast<uint64_t>(buf_.size()); }

    void DownloadLoop() {
        HandleCloser session;
        session.h = ::WinHttpOpen(ua_.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session.h) {
            Fail("WinHttpOpen", ::GetLastError());
            return;
        }
        ::WinHttpSetTimeouts(session.h, kConnectTimeoutMs, kConnectTimeoutMs, kConnectTimeoutMs, kReadTimeoutMs);

        HandleCloser conn;
        conn.h = ::WinHttpConnect(session.h, url_.host.c_str(), url_.port, 0);
        if (!conn.h) {
            Fail("WinHttpConnect", ::GetLastError());
            return;
        }

        HINTERNET req = ::WinHttpOpenRequest(conn.h, L"GET", url_.object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             url_.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!req) {
            Fail("WinHttpOpenRequest", ::GetLastError());
            return;
        }
        // 发布给 Cancel;之后这个句柄只由「谁持有谁关闭」的一方处理
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (canceled_) {
                ::WinHttpCloseHandle(req);
                return;
            }
            req_ = req;
        }

        // 明确要求 identity:WinHTTP 不会自动解 gzip,拿到压缩流会把音频数据解坏
        static const wchar_t kHeaders[] = L"Accept-Encoding: identity\r\nAccept: */*\r\n";
        bool ok = ::WinHttpSendRequest(req, kHeaders, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE;
        if (!ok) {
            Fail("WinHttpSendRequest", ::GetLastError());
        } else if (!::WinHttpReceiveResponse(req, nullptr)) {
            Fail("WinHttpReceiveResponse", ::GetLastError());
        } else {
            DWORD status = 0;
            DWORD len = sizeof(status);
            if (!::WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                       WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX)) {
                Fail("WinHttpQueryHeaders(状态码)", ::GetLastError());
            } else if (status < 200 || status >= 300) {
                Fail("HTTP 状态 " + std::to_string(status), 0);
            } else {
                ReadContentLength(req);
                ReadLoop(req);
            }
        }

        // 只有还在线程手里才关;已被 Cancel 收走的话那边已经关过了
        if (TakeBackRequest(req)) ::WinHttpCloseHandle(req);
    }

    void ReadContentLength(HINTERNET req) {
        DWORD total = 0;
        DWORD len = sizeof(total);
        // 分块传输(chunked)没有 Content-Length,拿不到就保持 0 = 未知
        if (::WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &total, &len, WINHTTP_NO_HEADER_INDEX)) {
            std::lock_guard<std::mutex> lk(mu_);
            total_ = static_cast<uint64_t>(total);
        }
    }

    void ReadLoop(HINTERNET req) {
        std::vector<uint8_t> chunk(kReadChunk);
        int idle = 0;
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (canceled_) return;
            }
            DWORD got = 0;
            if (!::WinHttpReadData(req, chunk.data(), kReadChunk, &got)) {
                const DWORD code = ::GetLastError();
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (canceled_) return; // 是我们自己关的句柄,不是故障
                }
                // 读超时多半只是网络停顿,连续多次才算断流
                if (code == ERROR_WINHTTP_TIMEOUT && idle < kMaxIdleTimeouts) {
                    ++idle;
                    continue;
                }
                Fail("WinHttpReadData", code);
                return;
            }
            idle = 0;
            if (got == 0) { // 服务端正常收尾
                MarkFinished();
                return;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (canceled_) return;
                // append-only:只往后长,offset 才能从 0 连续,不做整文件预分配
                buf_.insert(buf_.end(), chunk.data(), chunk.data() + got);
            }
            cv_.notify_all();
        }
    }

    // 请求句柄在线程与 Cancel 之间只能有一方关闭
    bool TakeBackRequest(HINTERNET req) {
        std::lock_guard<std::mutex> lk(mu_);
        if (req_ != req) return false;
        req_ = nullptr;
        return true;
    }

    void MarkFinished() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (canceled_ || failed_) return;
            finished_ = true;
        }
        cv_.notify_all();
    }

    void Fail(std::string stage, DWORD code) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (canceled_ || failed_) return;
            failed_ = true;
            error_ = std::move(stage);
            if (code != 0) error_ += "(GetLastError=" + std::to_string(code) + ")";
        }
        cv_.notify_all(); // 叫醒等待者,否则它们会一直等到超时
    }

    ParsedUrl url_;
    std::wstring ua_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<uint8_t> buf_; // append-only,受 mu_ 保护
    uint64_t total_ = 0;
    bool finished_ = false;
    bool failed_ = false;
    bool canceled_ = false;
    std::string error_;

    HINTERNET req_ = nullptr; // 仅用于让 Cancel 打断阻塞读,同样受 mu_ 保护
    std::thread worker_;
};

} // namespace

std::shared_ptr<StreamSource> OpenHttpStream(const std::string& url, const std::string& userAgent,
                                             std::string* error) {
    if (error) error->clear();

    // 地址不合法属于调用方错误,同步报出来比让后台线程慢慢失败好定位
    ParsedUrl parsed;
    if (!ParseUrl(url, parsed, error)) return nullptr;

    const std::string ua = userAgent.empty() ? std::string(kDefaultUa) : userAgent;
    auto source = std::make_shared<HttpStreamSource>(std::move(parsed), Utf8ToWide(ua));
    try {
        source->Start();
    } catch (const std::exception& e) {
        if (error) *error = std::string("后台下载线程创建失败: ") + e.what();
        return nullptr;
    }
    return source; // 立刻返回,网络相关的失败通过 Failed()/Error() 反映
}

} // namespace stream_core
