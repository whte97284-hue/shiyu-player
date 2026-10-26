#pragma once
#include <vector>
#include <cstdint>
#include <mutex>
#include <random>

enum class PlayMode {
    Sequential, // 顺序播放 (末尾停止)
    RepeatAll,  // 列表循环
    RepeatOne,  // 单曲循环
    Shuffle     // 随机洗牌 (Fisher-Yates 算法全列表置乱)
};

class PlayQueueController {
public:
    PlayQueueController();
    ~PlayQueueController() = default;

    // 队列设置与管理
    void SetQueue(const std::vector<uint64_t>& trackIds, size_t startIndex = 0);
    void Append(uint64_t trackId);
    void AppendBatch(const std::vector<uint64_t>& trackIds);
    void InsertNext(uint64_t trackId); // 下一首插播
    bool Remove(size_t index);
    void Clear();

    // 播放模式控制
    void SetPlayMode(PlayMode mode);
    PlayMode GetPlayMode() const;

    // 切歌状态机核心
    bool HasNext() const;
    bool HasPrevious() const;

    // 获取下一首/上一首的 Track ID (并同步推进当前播放状态)
    uint64_t Next();
    uint64_t Previous();

    // 直接跳转到指定索引
    bool JumpToIndex(size_t index);

    // 状态与查询
    uint64_t GetCurrentTrackId() const;
    int GetCurrentIndex() const; // 返回在原始队列中的索引位置，-1 表示未处于播放状态
    size_t GetQueueSize() const;
    std::vector<uint64_t> GetCurrentQueue() const;

private:
    void Reshuffle();

    mutable std::mutex mtx_;
    PlayMode mode_ = PlayMode::RepeatAll;

    std::vector<uint64_t> queue_;           // 原始队列
    std::vector<size_t> shuffle_order_;     // 随机洗牌映射序列
    int current_order_pos_ = -1;            // 在当前播放映射中的游标位置

    std::vector<uint64_t> history_stack_;   // 物理播放历史回退栈 (最大保留 50 首)
    std::mt19937 rng_;
};
