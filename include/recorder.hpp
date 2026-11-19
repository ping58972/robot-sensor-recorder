#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace capture {

struct Record {
    std::uint64_t sequence;
    std::uint64_t timestamp_ns;
    std::uint32_t stream;
    std::vector<std::uint8_t> payload;
};

// close() wakes blocked producers/consumers; already queued records remain drainable.
template<class T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : slots_(capacity)
    {
        if (capacity == 0) throw std::invalid_argument("Queue capacity must be positive.");
    }
    bool push(T value)
    {
        std::unique_lock lock(mutex_);
        writable_.wait(lock, [&] { return closed_ || count_ < slots_.size(); });
        if (closed_) return false;
        slots_[tail_] = std::move(value);
        tail_ = (tail_ + 1) % slots_.size();
        ++count_;
        peak_ = std::max(peak_, count_);
        readable_.notify_one();
        return true;
    }
    std::optional<T> pop()
    {
        std::unique_lock lock(mutex_);
        readable_.wait(lock, [&] { return closed_ || count_ != 0; });
        if (count_ == 0) return std::nullopt;
        auto value = std::move(slots_[head_]);
        slots_[head_].reset();
        head_ = (head_ + 1) % slots_.size();
        --count_;
        writable_.notify_one();
        return value;
    }
    void close()
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        readable_.notify_all();
        writable_.notify_all();
    }
    std::size_t peak() const
    {
        std::lock_guard lock(mutex_);
        return peak_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable readable_, writable_;
    std::vector<std::optional<T>> slots_;
    std::size_t head_ = 0, tail_ = 0, count_ = 0, peak_ = 0;
    bool closed_ = false;
};

class SegmentWriter {
public:
    explicit SegmentWriter(const std::filesystem::path& directory,
                           std::size_t records_per_segment = 32);
    ~SegmentWriter();
    SegmentWriter(const SegmentWriter&) = delete;
    SegmentWriter& operator=(const SegmentWriter&) = delete;
    void append(const Record& record);
    void finish();

private:
    void open_segment();
    void commit();
    std::filesystem::path directory_, partial_, committed_;
    std::size_t limit_, in_segment_ = 0, segment_ = 0;
    int fd_ = -1;
    bool finished_ = false;
};

struct Scan {
    std::uint64_t records = 0;
    std::uint64_t valid_bytes = 0;
    bool complete = true;
    std::string error;
};

Scan scan_file(const std::filesystem::path& file);
std::uint64_t verify_directory(const std::filesystem::path& directory);
std::uint64_t monotonic_ns();
std::uint64_t number(const std::string& text, std::uint64_t maximum);
void record_udp(const std::filesystem::path& directory, std::uint16_t port, std::size_t count);
void record_serial(const std::filesystem::path& directory, const std::string& device, std::size_t count);

} // namespace capture
