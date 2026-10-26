#pragma once
#include <string>
#include <vector>

// ============================================================================
//  CoverWriter —— 多容器封面写入 (自包含,零第三方依赖)
//
//  覆盖范围与容器差异:
//    FLAC      独立 PICTURE 块                        (见 cover_fetcher.cpp)
//    MP3       ID3v2.3 的 APIC 帧,保留原有其它帧
//    WAV       RIFF 里的 "id3 " 块 (内容是 ID3v2.3)
//    DSF       ID3v2.3 追加在文件尾,回填头部的元数据指针与文件大小
//    DSDIFF    "ID3 " 块 (大端 64 位长度),回填 FRM8 大小
//    OGG       Vorbis Comment 的 METADATA_BLOCK_PICTURE (FLAC PICTURE 块体 base64),
//              需重新分页并重算每页 CRC32 —— 图片几十万字节会撑爆原来那一页
//
//  ID3v2.3 这一套是共用的:MP3 / WAV / DSF / DSDIFF 都只是"把同一份 ID3 数据
//  放到容器的不同位置",所以只需要一个构造器 + 四种摆放方式。
//
//  不支持:MP4 (需解析 atom 树)、APE (APEv2 键值需重排)、Opus/Speex 的 OGG。
//  这几个在 UI 里会被标成"补不了"。
// ============================================================================

namespace cover_core {

// 这个扩展名能不能写入封面
bool CanEmbedCover(const std::wstring& path);

// 按扩展名分派。mime 为空则自动嗅探。写成功后原文件被原子替换。
// 返回 false 时 error 给出具体原因(不会损坏原文件)。
bool EmbedCover(const std::wstring& path, const std::string& image, const std::string& mime,
                std::string* error = nullptr);

// ---- 以下为分类型实现,便于单独测试 ----
bool EmbedMp3(const std::wstring& path, const std::string& image, const std::string& mime, std::string* error);
bool EmbedWav(const std::wstring& path, const std::string& image, const std::string& mime, std::string* error);
bool EmbedDsf(const std::wstring& path, const std::string& image, const std::string& mime, std::string* error);
bool EmbedDff(const std::wstring& path, const std::string& image, const std::string& mime, std::string* error);
bool EmbedOgg(const std::wstring& path, const std::string& image, const std::string& mime, std::string* error);

// 构造一个只含 APIC 的 ID3v2.3 标签块(含 10 字节头)
std::string BuildId3v23Apic(const std::string& image, const std::string& mime);

// 给已有的 ID3v2 数据(可为空)换上新的 APIC,保留其它帧。
// 传入的是完整 ID3v2 块(含头);返回新的完整块。
std::string ReplaceId3v2Apic(const std::string& oldTag, const std::string& image, const std::string& mime);

// 返回 ID3v2 块的总长度(含头);不是 ID3 返回 0
size_t Id3v2TotalSize(const std::string& data);

} // namespace cover_core
