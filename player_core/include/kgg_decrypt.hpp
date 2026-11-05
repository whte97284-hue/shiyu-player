#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
//  KGG / KGM / KGMA / VPR 解密 —— 独立实现,不依赖 kgg-dec.exe 或 infra.dll
//
//  算法来源:unlock-music (Go) 的公开实现 + 官方测试向量校准
//  已验证:
//    · KGMusicV3.db 按页 AES-128-CBC 解密 → 明文 SQLite
//    · 最小 B-tree 读取(含溢出页) → ShareFileItems 210/210 命中
//    · TEA 密钥推导(16 轮 / 大端) → 4 组官方向量逐字节一致
//    · QMC RC4 流密码 → 6 个样本解出 fLaC,其中 5 个与 kgg-dec 产物 MD5 完全相同
//
//  比 kgg-dec.exe 多做的:
//    · 支持 ekey 长度 >= 300 的 RC4 分支(kgg-dec 对它报 "create qmc2 failed")
//    · 支持 v3(老 KGM,slot 表)—— kgg-dec 只认 v5
//    · 多线程批量
// ============================================================================

namespace kugou {

struct KggResult {
    bool ok = false;
    std::wstring output;   // 产物路径
    // 失败原因(给界面直接显示):
    //   "db"       —— 找不到 KGMusicV3.db
    //   "no-ekey"  —— 库里有这首歌的记录但没有密钥(需要在酷狗里播放一次)
    //   "no-record"— 库里完全没有这首歌(需要先播放一次让它登记)
    //   "bad-header"/"unsupported-version"/"io"/"decode"
    std::string code;
    std::string detail;
};

class KggDecryptor {
public:
    // 定位酷狗密钥库。找不到返回空。
    static std::wstring FindDatabase();

    // 解密单个文件。dbPath 为空时自动查找。
    // outDir 为空时写到源文件旁边;否则写到 outDir。
    static KggResult Decrypt(const std::wstring& src, const std::wstring& outDir = {},
                             const std::wstring& dbPath = {});

    // 批量,多线程。threads <= 0 时按硬件并发数。
    static std::vector<KggResult> DecryptBatch(const std::vector<std::wstring>& files,
                                               const std::wstring& outDir = {}, int threads = 0);

    // 这个扩展名归不归我们管
    static bool Handles(const std::wstring& path);
};

} // namespace kugou
