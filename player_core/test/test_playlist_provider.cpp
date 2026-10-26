// 歌单解析验证 harness
// 用法: pltest [平台:ID 或 分享链接 ...]
// 不带参数则跑内置用例
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "playlist_provider.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace playlist_core;

static const char* PlatStr(Platform p) {
    switch (p) {
        case Platform::Kugou:   return "kugou";
        case Platform::NetEase: return "netease";
        case Platform::QQMusic: return "qqmusic";
        default:                return "unknown";
    }
}

// argv 中文按 ANSI (GBK) 传入, 转 UTF-8
static std::string ArgToUtf8(const char* a) {
    int n = MultiByteToWideChar(CP_ACP, 0, a, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_ACP, 0, a, -1, &w[0], n);
    return PlaylistProvider::WideToUtf8(w);
}

static void Dump(const std::string& input) {
    printf("\n============================================================\n");
    printf("输入: %s\n", input.c_str());

    PlaylistProvider pp;
    PlaylistResult r = pp.Parse(input);

    if (!r.ok) {
        printf("  [失败] %s\n", r.error.c_str());
        return;
    }
    printf("  平台: %s   歌单ID: %s\n", r.meta.source.c_str(), r.meta.id.c_str());
    printf("  歌单名: %s\n", PlaylistProvider::WideToUtf8(r.meta.name).c_str());
    if (!r.meta.author.empty())
        printf("  创建者: %s\n", PlaylistProvider::WideToUtf8(r.meta.author).c_str());
    if (!r.meta.cover_url.empty())
        printf("  封面:   %s\n", r.meta.cover_url.c_str());
    printf("  标称总数: %d   实解析: %zu   带时长: %d   带专辑: %d\n",
           r.meta.total, r.tracks.size(), r.with_duration, r.with_album);
    printf("  ------------------------------------------------------------\n");
    size_t maxshow = r.tracks.size() < 8 ? r.tracks.size() : 8;
    for (size_t i = 0; i < maxshow; ++i) {
        const auto& t = r.tracks[i];
        printf("  %2zu. %-36s | %-22s | %6u ms | id=%s\n",
               i + 1,
               PlaylistProvider::WideToUtf8(t.title).c_str(),
               PlaylistProvider::WideToUtf8(t.artist).c_str(),
               t.duration_ms, t.id.c_str());
        if (!t.album.empty())
            printf("      专辑: %s\n", PlaylistProvider::WideToUtf8(t.album).c_str());
    }
    if (r.tracks.size() > maxshow)
        printf("  ... 其余 %zu 首省略\n", r.tracks.size() - maxshow);
}

int main(int argc, char** argv) {
    // 输入归一化单测
    printf("### 输入归一化自测 ###\n");
    struct Case { const char* in; };
    const char* cases[] = {
        "https://www.kugou.com/yy/special/single/1067062.html",
        "https://m.kugou.com/plist/list/6222311?json=true",
        "https://m.kugou.com/share/zlist.html?listid=4761867",
        "https://www.kugou.com/share/xxxxx?specialid=4761867",
        "https://www.kugou.com/share/xxxxx?global_collection_id=collection_3_1127147424_165_0",
        // ⚠ 新版分享链接: 同时含 listid=3 (列表类型, 误导) 和 global_collection_id (真 ID)
        //   必须优先取 global_collection_id
        "http://wwwapi.kugou.com/share/zlist.html?chain=3RB9v52G6V2&from=pcCode&global_collection_id=collection_3_1016644002_3_0&listid=3&share_type=collect&type=0&uid=1016644002",
        // 短链 (需联网跟随 302)
        "https://t1.kugou.com/3RB9v52G6V2",
        "https://music.163.com/playlist?id=7480897649&userid=123",
        "https://music.163.com/#/playlist?id=7480897649",
        "https://y.music.163.com/m/playlist?id=7480897649",
        "https://music.163.com/playlist/7480897649/1/",
        "https://music.163.com/#/my/m/music/playlist?id=7480897649",
        // ---- QQ音乐 ----
        "https://c6.y.qq.com/base/fcgi-bin/u?__=57XH6jdnq48F",   // 短链 (需联网跟随 302×3)
        "https://y.qq.com/n/ryqq_v2/playlist/8962418095",
        "https://i.y.qq.com/n2/m/share/details/taoge.html?ADTAG=pc_v17&channelId=10036163&id=8962418095&openinqqmusic=1",
        "https://i2.y.qq.com/n3/other/pages/details/playlist.html?ADTAG=pc_v17&channelId=10036163&id=8962418095&redirect_from=node_v2",
        // 整段分享文案 (前面还有文字, 需先抠 URL)
        "分享歌单《En》: https://c6.y.qq.com/base/fcgi-bin/u?__=57XH6jdnq48F 快来听!",
        "kugou:1067062",
        "netease:7480897649",
        "qq:8962418095",
        "7480897649",
    };
    for (const char* c : cases) {
        Platform p = Platform::Unknown; std::string id;
        bool ok = PlaylistProvider::ParseInput(c, p, id);
        printf("  %-62s -> %-8s %s\n", c, ok ? PlatStr(p) : "FAIL", id.c_str());
    }

    printf("\n### 实际解析 ###\n");
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) Dump(ArgToUtf8(argv[i]));
    } else {
        Dump("https://www.kugou.com/yy/special/single/1067062.html");
        Dump("https://t1.kugou.com/3RB9v52G6V2");        // 新版短链 → gcid 全量
        Dump("#40717681#");                              // 酷狗码 (应给明确提示)
        Dump("https://music.163.com/playlist?id=7480897649");
        Dump("https://c6.y.qq.com/base/fcgi-bin/u?__=57XH6jdnq48F");  // QQ音乐短链
    }
    printf("\n");
    return 0;
}
