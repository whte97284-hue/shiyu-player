#include "../include/playlist_manager.hpp"
#include "../include/audio_tag_parser.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

PlaylistManager::PlaylistManager() = default;

std::string PlaylistManager::GenerateId() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return "pl_" + std::to_string(ms) + "_" + std::to_string(++id_counter_);
}

std::string PlaylistManager::CreatePlaylist(const std::wstring& name, const std::wstring& desc) {
    std::lock_guard<std::mutex> lock(mtx_);
    Playlist pl;
    pl.id = GenerateId();
    pl.name = name.empty() ? L"未命名歌单" : name;
    pl.description = desc;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    pl.created_at = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    pl.updated_at = pl.created_at;

    playlists_[pl.id] = pl;
    return pl.id;
}

bool PlaylistManager::DeletePlaylist(const std::string& playlistId) {
    std::lock_guard<std::mutex> lock(mtx_);
    return playlists_.erase(playlistId) > 0;
}

bool PlaylistManager::RenamePlaylist(const std::string& playlistId, const std::wstring& newName) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = playlists_.find(playlistId);
    if (it != playlists_.end()) {
        it->second.name = newName;
        auto now = std::chrono::system_clock::now().time_since_epoch();
        it->second.updated_at = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        return true;
    }
    return false;
}

bool PlaylistManager::SetDescription(const std::string& playlistId, const std::wstring& desc) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = playlists_.find(playlistId);
    if (it != playlists_.end()) {
        it->second.description = desc;
        auto now = std::chrono::system_clock::now().time_since_epoch();
        it->second.updated_at = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        return true;
    }
    return false;
}

const Playlist* PlaylistManager::GetPlaylist(const std::string& playlistId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = playlists_.find(playlistId);
    if (it != playlists_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<const Playlist*> PlaylistManager::GetAllPlaylists() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<const Playlist*> list;
    list.reserve(playlists_.size());
    for (const auto& pair : playlists_) {
        list.push_back(&pair.second);
    }
    return list;
}

size_t PlaylistManager::GetPlaylistCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return playlists_.size();
}

bool PlaylistManager::AddTrack(const std::string& playlistId, uint64_t trackId) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = playlists_.find(playlistId);
    if (it != playlists_.end()) {
        it->second.track_ids.push_back(trackId);
        auto now = std::chrono::system_clock::now().time_since_epoch();
        it->second.updated_at = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        return true;
    }
    return false;
}

bool PlaylistManager::AddTracks(const std::string& playlistId, const std::vector<uint64_t>& trackIds) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = playlists_.find(playlistId);
    if (it != playlists_.end()) {
        it->second.track_ids.insert(it->second.track_ids.end(), trackIds.begin(), trackIds.end());
        auto now = std::chrono::system_clock::now().time_since_epoch();
        it->second.updated_at = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        return true;
    }
    return false;
}

bool PlaylistManager::RemoveTrack(const std::string& playlistId, size_t index) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = playlists_.find(playlistId);
    if (it != playlists_.end() && index < it->second.track_ids.size()) {
        it->second.track_ids.erase(it->second.track_ids.begin() + index);
        auto now = std::chrono::system_clock::now().time_since_epoch();
        it->second.updated_at = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        return true;
    }
    return false;
}

bool PlaylistManager::MoveTrack(const std::string& playlistId, size_t fromIndex, size_t toIndex) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = playlists_.find(playlistId);
    if (it != playlists_.end()) {
        auto& ids = it->second.track_ids;
        if (fromIndex < ids.size() && toIndex < ids.size() && fromIndex != toIndex) {
            uint64_t val = ids[fromIndex];
            ids.erase(ids.begin() + fromIndex);
            ids.insert(ids.begin() + toIndex, val);
            auto now = std::chrono::system_clock::now().time_since_epoch();
            it->second.updated_at = std::chrono::duration_cast<std::chrono::seconds>(now).count();
            return true;
        }
    }
    return false;
}

void PlaylistManager::ClearPlaylist(const std::string& playlistId) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = playlists_.find(playlistId);
    if (it != playlists_.end()) {
        it->second.track_ids.clear();
        auto now = std::chrono::system_clock::now().time_since_epoch();
        it->second.updated_at = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    }
}

void PlaylistManager::ClearAll() {
    std::lock_guard<std::mutex> lock(mtx_);
    playlists_.clear();
}

bool PlaylistManager::ExportM3U8(const std::string& playlistId, const std::wstring& exportPath, const MusicCatalog& catalog) const {
    const Playlist* pl = GetPlaylist(playlistId);
    if (!pl) return false;

    // 构建 ID 到 TrackItem 映射
    std::unordered_map<uint64_t, const TrackItem*> trackMap;
    for (const auto& t : catalog.GetAllTracks()) {
        trackMap[t.id] = &t;
    }

    std::ofstream ofs(std::filesystem::path(exportPath), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!ofs.is_open()) return false;

    // UTF-8 BOM
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    ofs.write(reinterpret_cast<const char*>(bom), 3);

    std::string header = "#EXTM3U\r\n#PLAYLIST:" + AudioTagParser::WideToUtf8(pl->name) + "\r\n";
    ofs.write(header.data(), header.size());

    for (uint64_t id : pl->track_ids) {
        auto it = trackMap.find(id);
        if (it != trackMap.end()) {
            const TrackItem* t = it->second;
            int durSec = static_cast<int>(t->duration_ms / 1000);
            std::string line = "#EXTINF:" + std::to_string(durSec) + "," +
                               AudioTagParser::WideToUtf8(t->artist) + " - " + AudioTagParser::WideToUtf8(t->title) + "\r\n" +
                               AudioTagParser::WideToUtf8(t->file_path) + "\r\n";
            ofs.write(line.data(), line.size());
        }
    }

    ofs.close();
    return true;
}

bool PlaylistManager::ImportM3U8(const std::wstring& m3u8Path, MusicCatalog& catalog, std::string& outPlaylistId) {
    fs::path p(m3u8Path);
    if (!fs::exists(p)) return false;

    std::ifstream ifs(std::filesystem::path(m3u8Path), std::ios::binary);
    if (!ifs.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    // 跳过 BOM
    size_t start = 0;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF) {
        start = 3;
    }

    std::wstring plName = p.stem().wstring();
    std::vector<std::wstring> audioPaths;

    std::istringstream stream(content.substr(start));
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;

        if (line.rfind("#PLAYLIST:", 0) == 0) {
            plName = AudioTagParser::Utf8ToWide(line.substr(10));
            continue;
        }
        if (line[0] == '#') continue; // 跳过 #EXTINF 等标记

        std::wstring fPath = AudioTagParser::Utf8ToWide(line);
        // 若为相对路径，基于 M3U8 文件所在目录解析为绝对路径
        fs::path resolved(fPath);
        if (resolved.is_relative()) {
            resolved = p.parent_path() / resolved;
        }
        if (fs::exists(resolved)) {
            audioPaths.push_back(resolved.wstring());
        }
    }

    // 建立现存路径索引
    std::unordered_map<std::wstring, uint64_t> pathIdMap;
    for (const auto& t : catalog.GetAllTracks()) {
        pathIdMap[t.file_path] = t.id;
    }

    std::vector<uint64_t> importedIds;
    for (const auto& ap : audioPaths) {
        auto it = pathIdMap.find(ap);
        if (it != pathIdMap.end()) {
            importedIds.push_back(it->second);
        } else {
            // 自动入库
            AudioMetadata meta;
            if (AudioTagParser::ParseFile(ap, meta)) {
                catalog.AddTrack(ap, meta);
                // 最新添加项位于末尾
                if (!catalog.GetAllTracks().empty()) {
                    uint64_t newId = catalog.GetAllTracks().back().id;
                    importedIds.push_back(newId);
                    pathIdMap[ap] = newId;
                }
            }
        }
    }

    outPlaylistId = CreatePlaylist(plName, L"从 M3U8 文件导入");
    AddTracks(outPlaylistId, importedIds);
    return true;
}

bool PlaylistManager::SaveToJson(const std::wstring& filePath) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::ofstream ofs(std::filesystem::path(filePath), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!ofs.is_open()) return false;

    std::ostringstream ss;
    ss << "{\n  \"playlists\": [\n";

    size_t pCount = 0;
    for (const auto& pair : playlists_) {
        const auto& pl = pair.second;
        if (pCount > 0) ss << ",\n";
        ss << "    {\n";
        ss << "      \"id\": \"" << pl.id << "\",\n";
        ss << "      \"name\": \"" << AudioTagParser::WideToUtf8(pl.name) << "\",\n";
        ss << "      \"description\": \"" << AudioTagParser::WideToUtf8(pl.description) << "\",\n";
        ss << "      \"created_at\": " << pl.created_at << ",\n";
        ss << "      \"updated_at\": " << pl.updated_at << ",\n";
        ss << "      \"track_ids\": [";
        for (size_t i = 0; i < pl.track_ids.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << pl.track_ids[i];
        }
        ss << "]\n    }";
        pCount++;
    }

    ss << "\n  ]\n}\n";
    std::string str = ss.str();
    ofs.write(str.data(), str.size());
    ofs.close();
    return true;
}

bool PlaylistManager::LoadFromJson(const std::wstring& filePath) {
    if (!fs::exists(filePath)) return false;
    std::ifstream ifs(std::filesystem::path(filePath), std::ios::binary);
    if (!ifs.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    std::lock_guard<std::mutex> lock(mtx_);
    playlists_.clear();

    // 轻量高效 JSON 提取解析
    size_t pos = 0;
    while ((pos = content.find("\"id\": \"", pos)) != std::string::npos) {
        pos += 7;
        size_t idEnd = content.find("\"", pos);
        if (idEnd == std::string::npos) break;
        std::string id = content.substr(pos, idEnd - pos);

        // 提取 name
        std::wstring name = L"歌单";
        size_t nPos = content.find("\"name\": \"", idEnd);
        if (nPos != std::string::npos) {
            nPos += 9;
            size_t nEnd = content.find("\"", nPos);
            if (nEnd != std::string::npos) {
                name = AudioTagParser::Utf8ToWide(content.substr(nPos, nEnd - nPos));
            }
        }

        // 提取 description
        std::wstring desc = L"";
        size_t dPos = content.find("\"description\": \"", idEnd);
        if (dPos != std::string::npos) {
            dPos += 16;
            size_t dEnd = content.find("\"", dPos);
            if (dEnd != std::string::npos) {
                desc = AudioTagParser::Utf8ToWide(content.substr(dPos, dEnd - dPos));
            }
        }

        // 提取 track_ids
        std::vector<uint64_t> ids;
        size_t tPos = content.find("\"track_ids\": [", idEnd);
        if (tPos != std::string::npos) {
            tPos += 14;
            size_t tEnd = content.find("]", tPos);
            if (tEnd != std::string::npos) {
                std::string numStr = content.substr(tPos, tEnd - tPos);
                std::istringstream nss(numStr);
                std::string token;
                while (std::getline(nss, token, ',')) {
                    while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.erase(token.begin());
                    while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.pop_back();
                    if (!token.empty()) {
                        try {
                            ids.push_back(std::stoull(token));
                        } catch (...) {}
                    }
                }
            }
            pos = tEnd;
        }

        Playlist pl;
        pl.id = id;
        pl.name = name;
        pl.description = desc;
        pl.track_ids = ids;
        playlists_[id] = pl;
    }

    return true;
}
