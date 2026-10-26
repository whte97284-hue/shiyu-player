#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <cstdint>
#include "music_catalog.hpp"

struct Playlist {
    std::string id;                         // 歌单唯一 ID
    std::wstring name;                      // 歌单名称
    std::wstring description;               // 歌单描述
    std::vector<uint64_t> track_ids;        // 关联曲目的 ID 序列 (无重复数据冗余)
    uint64_t created_at = 0;
    uint64_t updated_at = 0;
};

class PlaylistManager {
public:
    PlaylistManager();
    ~PlaylistManager() = default;

    // 歌单基础生命周期
    std::string CreatePlaylist(const std::wstring& name, const std::wstring& desc = L"");
    bool DeletePlaylist(const std::string& playlistId);
    bool RenamePlaylist(const std::string& playlistId, const std::wstring& newName);
    bool SetDescription(const std::string& playlistId, const std::wstring& desc);

    const Playlist* GetPlaylist(const std::string& playlistId) const;
    std::vector<const Playlist*> GetAllPlaylists() const;
    size_t GetPlaylistCount() const;

    // 歌单内歌曲操作
    bool AddTrack(const std::string& playlistId, uint64_t trackId);
    bool AddTracks(const std::string& playlistId, const std::vector<uint64_t>& trackIds);
    bool RemoveTrack(const std::string& playlistId, size_t index);
    bool MoveTrack(const std::string& playlistId, size_t fromIndex, size_t toIndex);
    void ClearPlaylist(const std::string& playlistId);

    // 标准 M3U8 格式导入与导出 (支持全平台互通)
    bool ExportM3U8(const std::string& playlistId, const std::wstring& exportPath, const MusicCatalog& catalog) const;
    bool ImportM3U8(const std::wstring& m3u8Path, MusicCatalog& catalog, std::string& outPlaylistId);

    // 歌单集合持久化 (JSON 格式保存与读取)
    bool SaveToJson(const std::wstring& filePath) const;
    bool LoadFromJson(const std::wstring& filePath);

    void ClearAll();

private:
    std::string GenerateId();

    mutable std::mutex mtx_;
    std::unordered_map<std::string, Playlist> playlists_;
    uint64_t id_counter_ = 1000;
};
