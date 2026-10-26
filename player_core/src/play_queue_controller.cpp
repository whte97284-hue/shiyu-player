#include "../include/play_queue_controller.hpp"
#include <algorithm>
#include <chrono>

PlayQueueController::PlayQueueController() {
    auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    rng_.seed(static_cast<uint32_t>(seed));
}

void PlayQueueController::Reshuffle() {
    size_t sz = queue_.size();
    shuffle_order_.resize(sz);
    for (size_t i = 0; i < sz; ++i) shuffle_order_[i] = i;

    if (sz <= 1) {
        current_order_pos_ = 0;
        return;
    }

    // 确定当前首选并置于首位
    size_t currentOrigIdx = 0;
    if (current_order_pos_ >= 0 && current_order_pos_ < static_cast<int>(sz)) {
        currentOrigIdx = shuffle_order_[current_order_pos_];
    }

    // Fisher-Yates 随机置乱
    for (size_t i = sz - 1; i > 0; --i) {
        std::uniform_int_distribution<size_t> dist(0, i);
        size_t j = dist(rng_);
        std::swap(shuffle_order_[i], shuffle_order_[j]);
    }

    // 保持当前播放歌曲作为洗牌起步首项
    auto it = std::find(shuffle_order_.begin(), shuffle_order_.end(), currentOrigIdx);
    if (it != shuffle_order_.end()) {
        std::swap(shuffle_order_[0], *it);
    }
    current_order_pos_ = 0;
}

void PlayQueueController::SetQueue(const std::vector<uint64_t>& trackIds, size_t startIndex) {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_ = trackIds;
    history_stack_.clear();

    if (queue_.empty()) {
        current_order_pos_ = -1;
        shuffle_order_.clear();
        return;
    }

    if (startIndex >= queue_.size()) startIndex = 0;

    if (mode_ == PlayMode::Shuffle) {
        current_order_pos_ = static_cast<int>(startIndex);
        Reshuffle();
    } else {
        shuffle_order_.resize(queue_.size());
        for (size_t i = 0; i < queue_.size(); ++i) shuffle_order_[i] = i;
        current_order_pos_ = static_cast<int>(startIndex);
    }
}

void PlayQueueController::Append(uint64_t trackId) {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.push_back(trackId);
    shuffle_order_.push_back(queue_.size() - 1);
    if (current_order_pos_ < 0) {
        current_order_pos_ = 0;
    }
}

void PlayQueueController::AppendBatch(const std::vector<uint64_t>& trackIds) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (uint64_t id : trackIds) {
        queue_.push_back(id);
        shuffle_order_.push_back(queue_.size() - 1);
    }
    if (current_order_pos_ < 0 && !queue_.empty()) {
        current_order_pos_ = 0;
    }
}

void PlayQueueController::InsertNext(uint64_t trackId) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty() || current_order_pos_ < 0) {
        queue_.push_back(trackId);
        shuffle_order_.push_back(0);
        current_order_pos_ = 0;
        return;
    }

    // 插入到原始队列当前歌曲后
    size_t curOrig = shuffle_order_[current_order_pos_];
    queue_.insert(queue_.begin() + curOrig + 1, trackId);

    // 重新校准索引映射
    shuffle_order_.resize(queue_.size());
    for (size_t i = 0; i < queue_.size(); ++i) shuffle_order_[i] = i;
}

bool PlayQueueController::Remove(size_t index) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (index >= queue_.size()) return false;

    queue_.erase(queue_.begin() + index);
    if (queue_.empty()) {
        current_order_pos_ = -1;
        shuffle_order_.clear();
        return true;
    }

    if (mode_ == PlayMode::Shuffle) {
        Reshuffle();
    } else {
        shuffle_order_.resize(queue_.size());
        for (size_t i = 0; i < queue_.size(); ++i) shuffle_order_[i] = i;
        if (current_order_pos_ >= static_cast<int>(queue_.size())) {
            current_order_pos_ = static_cast<int>(queue_.size() - 1);
        }
    }
    return true;
}

void PlayQueueController::Clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.clear();
    shuffle_order_.clear();
    history_stack_.clear();
    current_order_pos_ = -1;
}

void PlayQueueController::SetPlayMode(PlayMode mode) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (mode_ == mode) return;

    mode_ = mode;
    if (queue_.empty()) return;

    if (mode_ == PlayMode::Shuffle) {
        Reshuffle();
    } else {
        // 恢复线性播放映射，但保持当前正在播放的曲目指针
        size_t curOrig = 0;
        if (current_order_pos_ >= 0 && current_order_pos_ < static_cast<int>(shuffle_order_.size())) {
            curOrig = shuffle_order_[current_order_pos_];
        }
        shuffle_order_.resize(queue_.size());
        for (size_t i = 0; i < queue_.size(); ++i) shuffle_order_[i] = i;
        current_order_pos_ = static_cast<int>(curOrig);
    }
}

PlayMode PlayQueueController::GetPlayMode() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return mode_;
}

bool PlayQueueController::HasNext() const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty()) return false;
    if (mode_ == PlayMode::RepeatAll || mode_ == PlayMode::RepeatOne || mode_ == PlayMode::Shuffle) {
        return true;
    }
    return (current_order_pos_ + 1 < static_cast<int>(queue_.size()));
}

bool PlayQueueController::HasPrevious() const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty()) return false;
    if (!history_stack_.empty()) return true;
    if (mode_ == PlayMode::RepeatAll || mode_ == PlayMode::Shuffle) return true;
    return (current_order_pos_ > 0);
}

uint64_t PlayQueueController::Next() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty()) return 0;

    // 记录历史回退栈
    if (current_order_pos_ >= 0 && current_order_pos_ < static_cast<int>(shuffle_order_.size())) {
        uint64_t currentId = queue_[shuffle_order_[current_order_pos_]];
        history_stack_.push_back(currentId);
        if (history_stack_.size() > 50) history_stack_.erase(history_stack_.begin());
    }

    if (mode_ == PlayMode::RepeatOne) {
        if (current_order_pos_ < 0) current_order_pos_ = 0;
        return queue_[shuffle_order_[current_order_pos_]];
    }

    if (mode_ == PlayMode::Sequential) {
        if (current_order_pos_ + 1 >= static_cast<int>(queue_.size())) {
            return 0; // 顺序播放结束
        }
        current_order_pos_++;
        return queue_[shuffle_order_[current_order_pos_]];
    }

    if (mode_ == PlayMode::RepeatAll) {
        current_order_pos_ = (current_order_pos_ + 1) % queue_.size();
        return queue_[shuffle_order_[current_order_pos_]];
    }

    if (mode_ == PlayMode::Shuffle) {
        current_order_pos_++;
        if (current_order_pos_ >= static_cast<int>(shuffle_order_.size())) {
            Reshuffle(); // 开启新一轮不重复随机洗牌
        }
        return queue_[shuffle_order_[current_order_pos_]];
    }

    return 0;
}

uint64_t PlayQueueController::Previous() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (queue_.empty()) return 0;

    // 优先从真实播放历史回退栈弹出上一首
    if (!history_stack_.empty()) {
        uint64_t prevId = history_stack_.back();
        history_stack_.pop_back();

        // 重新对齐当前游标
        for (size_t i = 0; i < shuffle_order_.size(); ++i) {
            if (queue_[shuffle_order_[i]] == prevId) {
                current_order_pos_ = static_cast<int>(i);
                break;
            }
        }
        return prevId;
    }

    // 历史栈为空时，物理游标回退
    if (current_order_pos_ > 0) {
        current_order_pos_--;
    } else {
        current_order_pos_ = static_cast<int>(queue_.size() - 1);
    }

    return queue_[shuffle_order_[current_order_pos_]];
}

bool PlayQueueController::JumpToIndex(size_t index) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (index >= queue_.size()) return false;

    // 记录历史
    if (current_order_pos_ >= 0 && current_order_pos_ < static_cast<int>(shuffle_order_.size())) {
        history_stack_.push_back(queue_[shuffle_order_[current_order_pos_]]);
    }

    for (size_t i = 0; i < shuffle_order_.size(); ++i) {
        if (shuffle_order_[i] == index) {
            current_order_pos_ = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

uint64_t PlayQueueController::GetCurrentTrackId() const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (current_order_pos_ >= 0 && current_order_pos_ < static_cast<int>(shuffle_order_.size())) {
        return queue_[shuffle_order_[current_order_pos_]];
    }
    return 0;
}

int PlayQueueController::GetCurrentIndex() const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (current_order_pos_ >= 0 && current_order_pos_ < static_cast<int>(shuffle_order_.size())) {
        return static_cast<int>(shuffle_order_[current_order_pos_]);
    }
    return -1;
}

size_t PlayQueueController::GetQueueSize() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return queue_.size();
}

std::vector<uint64_t> PlayQueueController::GetCurrentQueue() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return queue_;
}
