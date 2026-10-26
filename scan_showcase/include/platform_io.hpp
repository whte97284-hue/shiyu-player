#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <functional>

// ============================================================================
//  平台 I/O 抽象层:Windows 走 Win32 宽字符 API,其他平台走 POSIX。
//  解析内核、入库流水线与测试都只依赖这里,内核因此可以在任意平台编译与单测。
// ============================================================================

namespace platform {

// 只读、定位读取 (pread 语义) 的文件句柄,不移动共享文件指针,可安全跨线程复用。
class ReadOnlyFile {
public:
    ReadOnlyFile() = default;
    ~ReadOnlyFile();
    ReadOnlyFile(const ReadOnlyFile&) = delete;
    ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;

    // randomAccess = true 时提示系统关闭预读 (只读标签头时避免缓存管理器多读几 MB)
    bool Open(const std::wstring& path, bool randomAccess = true);
    void Close();
    bool IsOpen() const;
    uint64_t Size() const { return size_; }

    // 从 offset 读取至多 n 字节,返回实际读取字节数 (出错返回 0)
    size_t ReadAt(uint64_t offset, void* dst, size_t n) const;

private:
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
    uint64_t size_ = 0;
};

// 顺序写文件 (解密落盘使用)
class WritableFile {
public:
    WritableFile() = default;
    ~WritableFile();
    WritableFile(const WritableFile&) = delete;
    WritableFile& operator=(const WritableFile&) = delete;

    bool Create(const std::wstring& path);
    bool Write(const void* data, size_t n);
    void Close();

private:
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// 只读内存映射 (大文件,如 82MB 的 KGM 掩码表,整个运行期间只映射一次,多线程共享)
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool Open(const std::wstring& path);
    void Close();
    const uint8_t* Data() const { return data_; }
    uint64_t Size() const { return size_; }

private:
    const uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
#ifdef _WIN32
    void* file_ = nullptr;
    void* mapping_ = nullptr;
#endif
};

struct DirEntry {
    std::wstring path;     // 完整路径
    std::wstring name;     // 文件名
    uint64_t size = 0;
    uint64_t mtime = 0;    // 最后修改时间 (平台原生刻度,仅用于判断是否变化)
    bool is_directory = false;
};

// 单个文件的大小与修改时间 (失败返回 false)
bool GetFileInfo(const std::wstring& path, uint64_t& size, uint64_t& mtime);
// 原子替换:写临时文件后改名覆盖目标
bool AtomicReplaceFile(const std::wstring& from, const std::wstring& to);

// 高速递归目录遍历 (自动跳过 IsJunkDirectoryName / IsJunkFileName 条目)。Windows 下使用 FindFirstFileExW(FindExInfoBasic + LARGE_FETCH),
// 一次系统调用即可拿到文件名与大小,避免 std::filesystem 对每个文件的额外 stat。
// onDirectory(dirPath, entriesOfThatDir) 在每个目录枚举完毕后回调一次 (仅含文件)。
// 回调返回 false 时中止遍历。
bool EnumerateFilesRecursive(const std::wstring& root,
                             const std::function<bool(const std::wstring& dir, const std::vector<DirEntry>& files)>& onDirectory);

bool PathExists(const std::wstring& path);
bool IsDirectory(const std::wstring& path);
bool CreateDirectories(const std::wstring& path);
bool RemoveFile(const std::wstring& path);

// 探测路径所在物理磁盘是否存在寻道惩罚 (机械硬盘)。无法判断时返回 -1,机械盘 1,固态盘 0。
int QuerySeekPenalty(const std::wstring& path);

// Windows:转换为 \\?\ 扩展长度路径以突破 MAX_PATH;其他平台原样返回
std::wstring LongPath(const std::wstring& path);

// 目录遍历时应忽略的系统/垃圾条目:__MACOSX、AppleDouble (._*)、回收站、卷信息目录
bool IsJunkDirectoryName(const std::wstring& name);
bool IsJunkFileName(const std::wstring& name);

// 宽字符路径 <-> 本地路径
std::string NativePath(const std::wstring& path);

} // namespace platform
