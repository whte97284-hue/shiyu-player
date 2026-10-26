#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "audio_tag_parser.hpp"
#include "song_cleaner.hpp"
#include "pinyin_table.hpp"

struct TrackItem {
    uint64_t id = 0;
    std::wstring file_path;
    std::wstring file_name;
    // 解密产物入库时为加密源路径 (曲库按它归类、隐藏、移除);普通文件为空
    std::wstring source_path;

    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring album_artist;
    std::wstring genre;
    std::wstring year;
    std::wstring format; // "FLAC", "MP3", "NCM", "KGMA", "M4A", "DSF" ...
    std::wstring codec;  // "FLAC", "ALAC", "AAC", "DSD64" ...
    int track_no = 0;
    int disc_no = 0;

    uint32_t duration_ms = 0;
    uint32_t bitrate_kbps = 0;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint64_t file_size = 0;
    uint64_t added_time = 0; // 入库依据:文件修改时间 (平台原生单位,只用于比较先后)

    bool is_lossless = false;
    bool is_hires = false; // 采样率 > 48kHz 或位深 > 16bit,或 DSD
    bool has_cover = false;
    std::vector<uint8_t> cover_data; // 封面缓存 (惰性入库时为空,按需 MusicCatalog::LoadCover)
    std::string cover_mime;
    uint64_t cover_offset = 0;       // 内嵌封面物理偏移 (惰性定位)
    uint32_t cover_size = 0;
    std::wstring external_cover_path; // 同目录伴生封面 (cover.jpg / folder.jpg ...)

    // ReplayGain (dB / 线性峰值)
    bool has_rg_track = false;
    bool has_rg_album = false;
    float rg_track_gain = 0.0f;
    float rg_track_peak = 0.0f;
    float rg_album_gain = 0.0f;
    float rg_album_peak = 0.0f;

    // CUE 分轨:同一物理文件内的片段 (cue_end_ms == 0 表示到文件结尾)
    bool is_cue_track = false;
    uint32_t cue_start_ms = 0;
    uint32_t cue_end_ms = 0;

    // 拼音索引缓存
    std::string pinyin_title_full;
    std::string pinyin_title_abbr;
    std::string pinyin_artist_full;
    std::string pinyin_artist_abbr;
    std::string pinyin_album_full;
    std::string pinyin_album_abbr;
    std::string pinyin_key; // "'chen'yi'xun'..." 标题|歌手|专辑 拼音键,用于简繁/同音互搜

    // 预先小写化的检索字段
    std::wstring search_title;
    std::wstring search_artist;
    std::wstring search_album;
};

struct TrackGroup {
    const TrackItem* primary = nullptr;
    std::vector<const TrackItem*> duplicates;
};

struct CatalogStats {
    size_t total_tracks = 0;
    size_t total_artists = 0;
    size_t total_albums = 0;
    uint64_t total_duration_ms = 0;
    size_t lossless_count = 0;
    size_t with_cover_count = 0;
    size_t hires_count = 0;
    uint64_t total_size_bytes = 0;
};

class MusicCatalog {
public:
    MusicCatalog() = default;

    // 添加单首歌曲
    bool AddTrack(const std::wstring& filePath, const AudioMetadata& meta);

    // 线程安全的纯计算:清洗元数据并构建拼音/检索索引 (可在工作线程中并行执行,无需持有曲库锁)
    // sourcePath:解密产物对应的加密源 (无专辑标签时按源文件所在目录命名专辑,并记入 source_path)
    static TrackItem BuildTrack(const std::wstring& filePath, const AudioMetadata& meta, const std::wstring& sourcePath = L"");
    // 批量提交已构建好的曲目 (分配 ID 并建立倒排索引)
    void AddPreparedTracks(std::vector<TrackItem>&& tracks);

    // 智能全局检索 (deduplicate=true 时按歌手+歌名折叠同名多副本，优选高音质版本)
    // 结果按相关度排序:标题精确 > 标题前缀 > 标题包含 > 歌手/专辑 > 拼音
    std::vector<const TrackItem*> Search(const std::wstring& query, bool deduplicate = false) const;

    // 智能分组检索 (返回去重独立曲目与其名下所有物理实体副本)
    std::vector<TrackGroup> SearchGrouped(const std::wstring& query) const;

    // 按歌手筛选
    std::vector<const TrackItem*> FilterByArtist(const std::wstring& artist) const;

    // 按专辑筛选
    std::vector<const TrackItem*> FilterByAlbum(const std::wstring& album) const;

    // 按格式筛选 (如 "FLAC", "MP3", "KGMA", "KGG", "NCM")
    std::vector<const TrackItem*> FilterByFormat(const std::wstring& format) const;

    // 获取所有歌手列表
    std::vector<std::wstring> GetAllArtists() const;

    // 获取所有专辑列表
    std::vector<std::wstring> GetAllAlbums() const;

    // 获取曲库全部歌曲
    const std::vector<TrackItem>& GetAllTracks() const { return tracks_; }

    // 按 ID / 路径查找 (CUE 分轨同一路径对应多首,返回第一首)
    const TrackItem* FindById(uint64_t id) const;
    const TrackItem* FindByPath(const std::wstring& filePath) const;

    // 按需加载封面:内嵌封面按物理偏移一次读出,否则读取同目录伴生图片
    static bool LoadCover(const TrackItem& track, std::vector<uint8_t>& outData, std::string& outMime);

    // 获取曲库统计信息
    CatalogStats GetStats() const;

    // 清空曲库
    void Clear();

private:
    std::vector<TrackItem> tracks_;
    std::unordered_map<std::wstring, std::vector<size_t>> artist_index_;
    std::unordered_map<std::wstring, std::vector<size_t>> album_index_;
    std::unordered_map<std::wstring, std::vector<size_t>> format_index_;
    std::unordered_map<uint64_t, size_t> id_index_;
    std::unordered_map<std::wstring, size_t> path_index_;
    uint64_t next_id_ = 1;
};
