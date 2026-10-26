// 独立 harness:直接编译链接 lyric_provider.cpp,验证
// 1) 编译通过与工具函数正确性 (MD5/Base64/UrlEncode/JSON/Normalize)
// 2) 三源真实搜索 + 拉取 + 解析
#include "../include/lyric_provider.hpp"
#include "../include/audio_tag_parser.hpp"
#include "../include/lyric_manager.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// 命令行参数在中文 Windows 下是 GBK(ACP 936) 编码,不是 UTF-8。
// 直接 Utf8ToWide 会得到替换字符导致搜不到,这里按 ACP 解码。
static std::wstring ArgToWide(const char* s) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), 0);
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
#else
    return AudioTagParser::Utf8ToWide(s);
#endif
}

static void wprint(const std::wstring& w) {
    std::string u = AudioTagParser::WideToUtf8(w);
    std::cout << u;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::wstring title = L"富士山下";
    std::wstring artist = L"陈奕迅";
    if (argc >= 3) {
        artist = ArgToWide(argv[1]);
        title = ArgToWide(argv[2]);
    }

    // ---- 1. 工具函数自检 ----
    {
        std::string md5 = lyric_core::LyricProvider::Md5Hex("abc");
        std::cout << "[md5] abc -> " << md5 << " (expect 900150983cd24fb0d6963f7d28e17f72)\n";
        std::string dec;
        lyric_core::LyricProvider::Base64Decode("a3JjMQ==", dec);
        std::cout << "[b64] a3JjMQ== -> " << dec << " (expect krc1)\n";
        std::cout << "[url] " << lyric_core::LyricProvider::UrlEncode("东风破 A") << "\n";
        std::cout << "[lrc?] " << (lyric_core::LyricProvider::LooksLikeLrc("[00:12.34]hi") ? "yes" : "no") << "\n";
    }

    lyric_core::LyricQuery q;
    q.title = title;
    q.artist = artist;
    q.duration_ms = 0;

    lyric_core::LyricProvider prov;
    std::cout << "\n=== SearchAll: ";
    wprint(artist); std::cout << " - "; wprint(title); std::cout << " ===\n";
    auto res = prov.SearchAll(q);
    std::cout << "candidates: " << res.candidates.size() << "\n";
    for (auto& e : res.source_errors) std::cout << "  [err] " << e << "\n";
    size_t show = res.candidates.size() < 12 ? res.candidates.size() : 12;
    for (size_t i = 0; i < show; ++i) {
        auto& c = res.candidates[i];
        std::cout << "  #" << i << " [" << c.source << "] score=" << c.match_score << " id=" << c.id
                  << " dur=" << c.duration_ms << " synced=" << c.has_synced << " | ";
        wprint(c.artist); std::cout << " - "; wprint(c.title); std::cout << "\n";
    }

    // ---- 2. 逐个源验证 Fetch 与解析 ----
    for (const char* src : {"lrclib", "netease", "kugou"}) {
        std::vector<lyric_core::LyricCandidate> list;
        std::string err;
        std::cout << "\n--- fetch via " << src << " ---\n";
        if (!prov.Search(src, q, list, &err) || list.empty()) {
            std::cout << "  search failed: " << err << "\n";
            continue;
        }
        q.duration_ms = list[0].duration_ms;
        lyric_core::LyricProvider::RankCandidates(q, list);
        std::cout << "  best id=" << list[0].id << " dur=" << list[0].duration_ms << "\n";
        lyric_core::RawLyricDoc doc;
        if (!prov.Fetch(list[0], doc, &err)) {
            std::cout << "  fetch failed: " << err << "\n";
            continue;
        }
        std::cout << "  kind=" << (int)doc.kind << " lrc=" << doc.lrc_text.size()
                  << " tl=" << doc.translation_text.size() << " raw=" << doc.raw_payload.size() << "\n";
        lyric_core::ParsedLyrics parsed;
        bool ok = false;
        if (doc.kind == lyric_core::RawLyricDoc::Payload::Krc) {
            ok = lyric_core::KrcDecoder::DecodeMemory(
                reinterpret_cast<const uint8_t*>(doc.raw_payload.data()), doc.raw_payload.size(), parsed);
        } else if (doc.kind == lyric_core::RawLyricDoc::Payload::Lrc) {
            ok = lyric_core::KrcDecoder::ParseLrcText(doc.lrc_text, parsed);
        }
        std::cout << "  parsed=" << (ok ? "ok" : "FAIL") << " lines=" << parsed.lines.size() << "\n";
        for (size_t i = 0; i < parsed.lines.size() && i < 5; ++i) {
            std::cout << "    [" << parsed.lines[i].start_ms << "] ";
            wprint(parsed.lines[i].full_text);
            if (!parsed.lines[i].translation.empty()) { std::cout << " | "; wprint(parsed.lines[i].translation); }
            std::cout << "\n";
        }
    }

    std::cout << "\nDONE\n";
    return 0;
}
