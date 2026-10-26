#pragma once
#include <string>
#include <vector>
#include <regex>

struct CleanedSongInfo {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::vector<std::wstring> artists; // 分割后的多歌手列表
    bool was_cleaned = false;
};

class SongCleaner {
public:
    // 清洗歌曲信息，当标签缺失时回退提取文件名
    // folderName:所在专辑目录名 (跳过 CD1 / FLAC 等通用目录),没有歌手标签时用来推断歌手
    static CleanedSongInfo Clean(const std::wstring& rawTitle, 
                                 const std::wstring& rawArtist, 
                                 const std::wstring& rawAlbum, 
                                 const std::wstring& fileName,
                                 const std::wstring& folderName = L"");

    // 去除标题中的垃圾标记 (如 [FLAC], (320k), 【无损】, _kgg-dec 等)
    static std::wstring CleanTitle(const std::wstring& title);

    // 标准化艺术家名称并拆分多歌手
    static std::vector<std::wstring> SplitArtists(const std::wstring& artist);

    // 从文件名猜测歌手与歌名 (当元数据为空时)
    static bool GuessFromFileName(const std::wstring& fileName, std::wstring& outArtist, std::wstring& outTitle);

    // 去掉文件名开头紧贴歌名的补零曲号:"05东风破" → "东风破" (只认 0 开头,避免误伤「17岁」「2002年的第一场雪」)
    static std::wstring StripTrackPrefix(const std::wstring& name);

    // 从「歌手 专辑」「歌手 - 专辑」形式的目录名拆出歌手与专辑
    static bool GuessFromFolder(const std::wstring& folderName, std::wstring& outArtist, std::wstring& outAlbum);

    // 去除首尾空格与全角/半角标点噪声
    static std::wstring Trim(const std::wstring& str);
};
