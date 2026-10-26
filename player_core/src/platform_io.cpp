#include "../include/platform_io.hpp"
#include "../include/audio_tag_parser.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <cerrno>
#endif

#include <algorithm>
#include <climits>
#include <cstdio>

namespace platform {

bool IsJunkDirectoryName(const std::wstring& n) {
    return n == L"__MACOSX" || n == L"$RECYCLE.BIN" || n == L"System Volume Information" || n == L".Trashes" ||
           n == L".Spotlight-V100" || n == L".fseventsd";
}

bool IsJunkFileName(const std::wstring& n) {
    return (n.size() >= 2 && n[0] == L'.' && n[1] == L'_') || n == L".DS_Store" || n == L"Thumbs.db" ||
           n == L"desktop.ini";
}

#ifdef _WIN32

// 转换为 \\?\ 扩展长度路径,突破 MAX_PATH (260) 限制。
// 动漫原盘目录名常带完整专辑标题,嵌套两层后很容易超过 260 字符。
std::wstring LongPath(const std::wstring& path) {
    if (path.empty() || path.compare(0, 4, L"\\\\?\\") == 0 || path.compare(0, 4, L"\\\\.\\") == 0) return path;
    std::wstring full = path;
    DWORD need = GetFullPathNameW(path.c_str(), 0, NULL, NULL);
    if (need > 0) {
        std::wstring buf(need, L'\0');
        DWORD got = GetFullPathNameW(path.c_str(), need, &buf[0], NULL);
        if (got > 0 && got < need) full.assign(buf.c_str(), got);
    }
    for (auto& ch : full)
        if (ch == L'/') ch = L'\\';
    if (full.size() >= 2 && full[0] == L'\\' && full[1] == L'\\') return L"\\\\?\\UNC\\" + full.substr(2);
    if (full.size() >= 3 && full[1] == L':') return L"\\\\?\\" + full;
    return path;
}

ReadOnlyFile::~ReadOnlyFile() { Close(); }

bool ReadOnlyFile::Open(const std::wstring& path, bool randomAccess) {
    Close();
    DWORD flags = FILE_ATTRIBUTE_NORMAL | (randomAccess ? FILE_FLAG_RANDOM_ACCESS : FILE_FLAG_SEQUENTIAL_SCAN);
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, flags, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        return false;
    }
    handle_ = h;
    size_ = static_cast<uint64_t>(sz.QuadPart);
    return true;
}

void ReadOnlyFile::Close() {
    if (handle_) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    size_ = 0;
}

bool ReadOnlyFile::IsOpen() const { return handle_ != nullptr; }

size_t ReadOnlyFile::ReadAt(uint64_t offset, void* dst, size_t n) const {
    if (!handle_ || n == 0 || offset >= size_) return 0;
    size_t total = 0;
    uint8_t* out = static_cast<uint8_t*>(dst);
    while (total < n) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(n - total, 1u << 30));
        OVERLAPPED ov = {};
        uint64_t pos = offset + total;
        ov.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(pos >> 32);
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), out + total, chunk, &got, &ov) || got == 0) break;
        total += got;
    }
    return total;
}

WritableFile::~WritableFile() { Close(); }

bool WritableFile::Create(const std::wstring& path) {
    Close();
    HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    handle_ = h;
    return true;
}

bool WritableFile::Write(const void* data, size_t n) {
    if (!handle_) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (n > 0) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(n, 1u << 30));
        DWORD written = 0;
        if (!::WriteFile(static_cast<HANDLE>(handle_), p, chunk, &written, NULL) || written == 0) return false;
        p += written;
        n -= written;
    }
    return true;
}

void WritableFile::Close() {
    if (handle_) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

MappedFile::~MappedFile() { Close(); }

bool MappedFile::Open(const std::wstring& path) {
    Close();
    HANDLE f = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0) {
        CloseHandle(f);
        return false;
    }
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!m) {
        CloseHandle(f);
        return false;
    }
    const void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!v) {
        CloseHandle(m);
        CloseHandle(f);
        return false;
    }
    file_ = f;
    mapping_ = m;
    data_ = static_cast<const uint8_t*>(v);
    size_ = static_cast<uint64_t>(sz.QuadPart);
    return true;
}

void MappedFile::Close() {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
    data_ = nullptr;
    mapping_ = nullptr;
    file_ = nullptr;
    size_ = 0;
}

bool GetFileInfo(const std::wstring& path, uint64_t& size, uint64_t& mtime) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &d)) return false;
    size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    mtime = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    return true;
}

bool AtomicReplaceFile(const std::wstring& from, const std::wstring& to) {
    return MoveFileExW(LongPath(from).c_str(), LongPath(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool RemoveFile(const std::wstring& path) { return DeleteFileW(LongPath(path).c_str()) != 0; }

bool EnumerateFilesRecursive(const std::wstring& root,
                             const std::function<bool(const std::wstring&, const std::vector<DirEntry>&)>& onDirectory) {
    std::vector<std::wstring> stack;
    stack.push_back(root);
    std::vector<DirEntry> files;
    std::vector<std::wstring> subdirs;

    while (!stack.empty()) {
        std::wstring dir = std::move(stack.back());
        stack.pop_back();
        files.clear();
        subdirs.clear();

        std::wstring pattern = dir;
        if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
        std::wstring prefix = pattern;
        pattern += L'*';

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(LongPath(pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const wchar_t* name = fd.cFileName;
            if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue; // 不跟随联接点,防止环
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!IsJunkDirectoryName(name)) subdirs.push_back(prefix + name);
            } else if (!IsJunkFileName(name)) {
                DirEntry e;
                e.name = name;
                e.path = prefix + name;
                e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                e.mtime = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                files.push_back(std::move(e));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        if (!files.empty()) {
            std::sort(files.begin(), files.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
            if (!onDirectory(dir, files)) return false;
        }
        // 逆序压栈,保证按名称顺序深度优先遍历
        std::sort(subdirs.begin(), subdirs.end());
        for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) stack.push_back(std::move(*it));
    }
    return true;
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(LongPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
    DWORD a = GetFileAttributesW(LongPath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool CreateDirectories(const std::wstring& path) {
    if (path.empty()) return false;
    if (IsDirectory(path)) return true;
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos > 0) {
        std::wstring parent = path.substr(0, pos);
        if (!(parent.size() == 2 && parent[1] == L':')) CreateDirectories(parent);
    }
    return CreateDirectoryW(LongPath(path).c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

int QuerySeekPenalty(const std::wstring& path) {
    wchar_t volPath[MAX_PATH] = {0};
    if (!GetVolumePathNameW(path.c_str(), volPath, MAX_PATH)) return -1;
    std::wstring vol = volPath; // 形如 "F:\"
    if (vol.size() < 2 || vol[1] != L':') return -1;
    std::wstring dev = L"\\\\.\\" + vol.substr(0, 2);
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d = {};
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &d, sizeof(d), &ret, NULL);
    CloseHandle(h);
    if (!ok || ret < sizeof(d)) return -1;
    return d.IncursSeekPenalty ? 1 : 0;
}

std::string NativePath(const std::wstring& path) { return AudioTagParser::WideToUtf8(path); }

#else // ======================= POSIX =======================

std::string NativePath(const std::wstring& path) { return AudioTagParser::WideToUtf8(path); }

std::wstring LongPath(const std::wstring& path) { return path; }

ReadOnlyFile::~ReadOnlyFile() { Close(); }

bool ReadOnlyFile::Open(const std::wstring& path, bool randomAccess) {
    Close();
    int fd = ::open(NativePath(path).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return false;
    }
#if defined(POSIX_FADV_RANDOM)
    posix_fadvise(fd, 0, 0, randomAccess ? POSIX_FADV_RANDOM : POSIX_FADV_SEQUENTIAL);
#else
    (void)randomAccess;
#endif
    fd_ = fd;
    size_ = static_cast<uint64_t>(st.st_size);
    return true;
}

void ReadOnlyFile::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    size_ = 0;
}

bool ReadOnlyFile::IsOpen() const { return fd_ >= 0; }

size_t ReadOnlyFile::ReadAt(uint64_t offset, void* dst, size_t n) const {
    if (fd_ < 0 || n == 0 || offset >= size_) return 0;
    size_t total = 0;
    uint8_t* out = static_cast<uint8_t*>(dst);
    while (total < n) {
        ssize_t got = ::pread(fd_, out + total, n - total, static_cast<off_t>(offset + total));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        total += static_cast<size_t>(got);
    }
    return total;
}

WritableFile::~WritableFile() { Close(); }

bool WritableFile::Create(const std::wstring& path) {
    Close();
    fd_ = ::open(NativePath(path).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    return fd_ >= 0;
}

bool WritableFile::Write(const void* data, size_t n) {
    if (fd_ < 0) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (n > 0) {
        ssize_t w = ::write(fd_, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

void WritableFile::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

MappedFile::~MappedFile() { Close(); }

bool MappedFile::Open(const std::wstring& path) {
    Close();
    int fd = ::open(NativePath(path).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return false;
    }
    void* v = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (v == MAP_FAILED) return false;
    data_ = static_cast<const uint8_t*>(v);
    size_ = static_cast<uint64_t>(st.st_size);
    return true;
}

void MappedFile::Close() {
    if (data_) munmap(const_cast<uint8_t*>(data_), static_cast<size_t>(size_));
    data_ = nullptr;
    size_ = 0;
}

bool GetFileInfo(const std::wstring& path, uint64_t& size, uint64_t& mtime) {
    struct stat st;
    if (stat(NativePath(path).c_str(), &st) != 0) return false;
    size = static_cast<uint64_t>(st.st_size);
    mtime = static_cast<uint64_t>(st.st_mtim.tv_sec) * 1000000000ull + static_cast<uint64_t>(st.st_mtim.tv_nsec);
    return true;
}

bool AtomicReplaceFile(const std::wstring& from, const std::wstring& to) {
    return ::rename(NativePath(from).c_str(), NativePath(to).c_str()) == 0;
}

bool RemoveFile(const std::wstring& path) { return ::unlink(NativePath(path).c_str()) == 0; }

bool EnumerateFilesRecursive(const std::wstring& root,
                             const std::function<bool(const std::wstring&, const std::vector<DirEntry>&)>& onDirectory) {
    std::vector<std::wstring> stack;
    stack.push_back(root);
    std::vector<DirEntry> files;
    std::vector<std::wstring> subdirs;

    while (!stack.empty()) {
        std::wstring dir = std::move(stack.back());
        stack.pop_back();
        files.clear();
        subdirs.clear();

        std::string ndir = NativePath(dir);
        DIR* d = opendir(ndir.c_str());
        if (!d) continue;
        std::wstring prefix = dir;
        if (!prefix.empty() && prefix.back() != L'/') prefix += L'/';
        while (struct dirent* de = readdir(d)) {
            const char* n = de->d_name;
            if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) continue;
            std::string full = ndir + "/" + n;
            struct stat st;
            if (lstat(full.c_str(), &st) != 0) continue;
            if (S_ISLNK(st.st_mode)) continue;
            std::wstring wname = AudioTagParser::Utf8ToWide(n);
            if (S_ISDIR(st.st_mode)) {
                if (!IsJunkDirectoryName(wname)) subdirs.push_back(prefix + wname);
            } else if (S_ISREG(st.st_mode) && !IsJunkFileName(wname)) {
                DirEntry e;
                e.name = wname;
                e.path = prefix + wname;
                e.size = static_cast<uint64_t>(st.st_size);
                e.mtime = static_cast<uint64_t>(st.st_mtim.tv_sec) * 1000000000ull + static_cast<uint64_t>(st.st_mtim.tv_nsec);
                files.push_back(std::move(e));
            }
        }
        closedir(d);

        if (!files.empty()) {
            std::sort(files.begin(), files.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
            if (!onDirectory(dir, files)) return false;
        }
        std::sort(subdirs.begin(), subdirs.end());
        for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) stack.push_back(std::move(*it));
    }
    return true;
}

bool PathExists(const std::wstring& path) {
    struct stat st;
    return stat(NativePath(path).c_str(), &st) == 0;
}

bool IsDirectory(const std::wstring& path) {
    struct stat st;
    return stat(NativePath(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool CreateDirectories(const std::wstring& path) {
    if (path.empty()) return false;
    if (IsDirectory(path)) return true;
    size_t pos = path.find_last_of(L'/');
    if (pos != std::wstring::npos && pos > 0) CreateDirectories(path.substr(0, pos));
    return ::mkdir(NativePath(path).c_str(), 0755) == 0 || errno == EEXIST;
}

int QuerySeekPenalty(const std::wstring&) { return -1; }

#endif

} // namespace platform
