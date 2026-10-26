#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ============================================================================
//  整理曲库:按命名模板把歌曲移动 / 复制 / 改名到统一的目录结构 (参考 foobar2000 的 File Operations)
//
//  用法:BuildPlan 只计算不动文件 (界面据此做预览) → Execute 按计划执行 → 需要时用日志 Undo。
//
//  · 模板:%artist% %album artist% %album% %title% %tracknumber% %discnumber% %date% %year% %genre%
//          %codec% %format% %filename% %ext% %directoryname%;[ ... ] 内的字段全部为空时整段省略 (与 foobar2000 一致);
//          函数 $if2(a,b) $if3(a,b,...) $num(x,n) $left(x,n) $upper(x) $lower(x) $trim(x) $replace(x,a,b);
//          ' 引号内为原样文字;\ 与 / 都是目录分隔符。
//  · 字段值里的非法字符按选项替换为 _ 或全角字符;去掉结尾的点和空格;避开 CON / NUL 等保留名;
//    单级名称与整条路径超长时截断 (保留扩展名)。
//  · 同名附属文件 (.lrc .krc .cue .jpg ...) 随歌曲一起移动并改成同样的主文件名;
//    目录封面 (cover.jpg / folder.jpg ...) 在整个目录都去往同一个地方时一起移动,否则复制到每个去处。
//  · 整轨 + CUE:文件名保持不变 (CUE 里按文件名引用),只按模板决定所在目录,CUE 与整轨一起搬。
//  · 冲突:已在目标位置 → 跳过;两首歌指向同一路径 / 目标已有别的文件 → 内容相同记为重复 (按选项跳过),
//    不同则追加 (2)、(3);互相交换位置 (A→B、B→A)、仅大小写不同的改名都经临时名两步完成。
//  · 同一个盘直接改名 (瞬间完成);跨盘先复制到 .part、校验后改名,再删除源文件;执行前检查目标盘空间。
//  · 每一步写入日志 (先写计划,再逐条记完成),中途崩溃 / 断电后可以继续或撤销;Undo 逆序还原。
//  · 执行完成后给出「旧路径 → 新路径」映射,调用方据此更新收藏、歌单、播放记录、解密记录。
// ============================================================================

namespace organizer {

// 一个物理文件 (整轨 + CUE 也只算一个,用专辑级标签)
struct OrganizeTrack {
    std::wstring path;
    std::wstring title, artist, album_artist, album, genre, date, codec, format;
    int track_no = 0;
    int disc_no = 0;
    int disc_total = 0;  // 大于 1 时 %discnumber% 才有值 (单碟专辑不出现「1-」前缀)
    bool cue_image = false; // 整轨 + CUE:只按模板决定目录,文件名不变
};

enum class Mode {
    Move,          // 移动到目标根目录下
    Copy,          // 复制到目标根目录下,源文件不动
    RenameInPlace  // 留在原目录,只按模板的最后一级改文件名
};

enum class IllegalCharPolicy {
    Underscore, // \ / : * ? " < > | → _
    FullWidth   // → ＼ ／ ： ＊ ？ ＂ ＜ ＞ ｜
};

enum class DuplicatePolicy {
    KeepBoth, // 追加 (2)
    Skip      // 内容完全相同时不再搬运;移动模式下源文件保留在原处,在计划里标为重复
};

struct OrganizeOptions {
    Mode mode = Mode::Move;
    std::wstring target_root;   // Move / Copy 必填
    std::wstring pattern = L"$if2(%album artist%,%artist%)\\[%date% - ]%album%\\[%discnumber%-]$num(%tracknumber%,2) %title%";
    IllegalCharPolicy illegal = IllegalCharPolicy::Underscore;
    DuplicatePolicy duplicates = DuplicatePolicy::Skip;
    std::wstring unknown_value = L"未知";  // 某一级目录名渲染后为空时使用
    bool move_companions = true;          // 同名 .lrc / .krc / .cue / 图片
    bool move_folder_art = true;          // cover.jpg / folder.jpg 等
    bool remove_empty_dirs = true;        // 移动后清理变空的源目录 (只清理到 protect_roots 为止)
    bool verify_copy = true;              // 跨盘复制后逐字节比对
    size_t max_component = 150;           // 单级名称最大字符数
    size_t max_path = 259;                // 整条路径最大字符数 (0 = 不限制,系统已开启长路径时)
    std::vector<std::wstring> protect_roots;  // 曲库根目录:清理空目录时不会删到这些目录本身及其上级
    std::vector<std::wstring> exclude_paths;  // 不参与整理的文件或目录 (正在播放的歌、酷狗下载目录 ...)
};

enum class OpKind { Audio, Companion, FolderArt };
enum class OpAction { Move, Copy, Skip };
enum class OpState {
    Planned,
    Unchanged,   // 已经在目标位置
    Duplicate,   // 目标处已有内容相同的文件
    Renamed,     // 目标重名,已追加序号
    Excluded,
    Error        // 计划阶段就无法处理 (模板出错、源文件不存在 ...)
};

struct OrganizeOp {
    OpKind kind = OpKind::Audio;
    OpAction action = OpAction::Move;
    OpState state = OpState::Planned;
    std::wstring from, to;
    std::wstring note;
    uint64_t size = 0;
    size_t track = SIZE_MAX;    // 来源 OrganizeTrack 下标 (目录封面为 SIZE_MAX)
    bool cross_volume = false;  // 跨盘:复制 + 校验 + 删除
    bool via_temp = false;      // 目标正被另一个源文件占用 / 仅大小写不同:经临时名两步完成
};

struct VolumeNeed {
    std::wstring root;       // 目标所在盘
    uint64_t bytes = 0;      // 需要写入的字节 (跨盘复制 / 复制模式)
    uint64_t available = 0;  // 当前可用
    bool enough = true;
};

struct OrganizePlan {
    std::vector<OrganizeOp> ops;
    std::vector<VolumeNeed> volumes;
    std::vector<std::wstring> warnings;  // 截断、缺标签等
    size_t to_move = 0, to_copy = 0, unchanged = 0, duplicates = 0, renamed = 0, errors = 0, excluded = 0;
    bool cancelled = false;  // 计算计划时被取消:内容不完整,不能执行
    bool ok() const;  // 没有空间不足
};

struct OrganizeProgress {
    size_t done = 0;
    size_t total = 0;
    uint64_t bytes_done = 0;
    uint64_t bytes_total = 0;
    std::wstring current;
    int stage = 0;  // 计算计划时:0 = 算新位置,1 = 比对可能重复的文件
};

struct PathChange {
    std::wstring from, to;
    bool copied = false;  // 复制模式:源文件还在
};

struct OrganizeResult {
    size_t moved = 0, copied = 0, skipped = 0, failed = 0;
    bool cancelled = false;
    std::vector<PathChange> changes;          // 音频与附属文件的路径变化 (按执行顺序)
    std::vector<std::pair<std::wstring, std::wstring>> failures;  // 源路径, 原因
    std::vector<std::wstring> removed_dirs;   // 清理掉的空目录
    std::wstring journal;
};

// 渲染模板 (单元测试与界面预览用):返回相对路径 (不含扩展名),出错时返回空并写入 error
std::wstring FormatPattern(const std::wstring& pattern, const OrganizeTrack& t, const OrganizeOptions& opt,
                           std::wstring* error = nullptr);
// 检查模板语法,出错返回 false 并说明原因
bool ValidatePattern(const std::wstring& pattern, std::wstring* error = nullptr);

// 计算整理计划 (不动任何文件)。目标处撞名、大小相同的文件用抽样指纹判断是否重复 (多线程读取);
// cancel 时返回 cancelled = true 的不完整计划
OrganizePlan BuildPlan(const std::vector<OrganizeTrack>& tracks, const OrganizeOptions& opt,
                       std::function<void(const OrganizeProgress&)> onProgress = nullptr,
                       const std::atomic<bool>* cancel = nullptr);

// 执行计划。journalPath 为空时不写日志 (不能撤销)。cancel 在两个文件之间与复制过程中检查
OrganizeResult Execute(const OrganizePlan& plan, const OrganizeOptions& opt, const std::wstring& journalPath,
                       std::function<void(const OrganizeProgress&)> onProgress = nullptr,
                       const std::atomic<bool>* cancel = nullptr);

// 日志状态:是否存在未完成的整理 (上次中途退出)
struct JournalInfo {
    bool exists = false;
    bool finished = false;
    bool undone = false;
    size_t planned = 0;
    size_t completed = 0;
};
JournalInfo InspectJournal(const std::wstring& journalPath);

// 按日志逆序撤销已完成的操作:移动的搬回去,复制的删掉副本,再清理整理时新建的空目录
OrganizeResult Undo(const std::wstring& journalPath, std::function<void(const OrganizeProgress&)> onProgress = nullptr);

} // namespace organizer
