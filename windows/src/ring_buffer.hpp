#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fastaudio {

class PcmRingBuffer {
public:
    explicit PcmRingBuffer(uint32_t capacityFrames)
        : capacity_(capacityFrames), samples_(static_cast<size_t>(capacityFrames) * 2) {
    }

    uint32_t capacity() const {
        return capacity_;
    }

    uint32_t available() const {
        uint64_t write = write_.load(std::memory_order_acquire);
        uint64_t read = read_.load(std::memory_order_acquire);
        return static_cast<uint32_t>(std::min<uint64_t>(write - read, capacity_));
    }

    uint32_t push(const int16_t* source, uint32_t frames) {
        uint64_t write = write_.load(std::memory_order_relaxed);
        uint64_t read = read_.load(std::memory_order_acquire);
        uint32_t freeFrames = capacity_ - static_cast<uint32_t>(write - read);
        uint32_t accepted = std::min(frames, freeFrames);
        copyIn(write, source, accepted);
        write_.store(write + accepted, std::memory_order_release);
        return accepted;
    }

    uint32_t pop(int16_t* target, uint32_t frames) {
        uint64_t read = read_.load(std::memory_order_relaxed);
        uint64_t write = write_.load(std::memory_order_acquire);
        uint32_t count = std::min<uint32_t>(
                frames, static_cast<uint32_t>(write - read));
        copyOut(read, target, count);
        read_.store(read + count, std::memory_order_release);
        return count;
    }

    uint32_t discard(uint32_t frames) {
        uint64_t read = read_.load(std::memory_order_relaxed);
        uint64_t write = write_.load(std::memory_order_acquire);
        uint32_t count = std::min<uint32_t>(
                frames, static_cast<uint32_t>(write - read));
        read_.store(read + count, std::memory_order_release);
        return count;
    }

private:
    void copyIn(uint64_t position, const int16_t* source, uint32_t frames) {
        uint32_t index = static_cast<uint32_t>(position % capacity_);
        uint32_t first = std::min(frames, capacity_ - index);
        std::memcpy(&samples_[static_cast<size_t>(index) * 2], source,
                    static_cast<size_t>(first) * 2 * sizeof(int16_t));
        if (first < frames) {
            std::memcpy(samples_.data(), source + static_cast<size_t>(first) * 2,
                        static_cast<size_t>(frames - first) * 2 * sizeof(int16_t));
        }
    }

    void copyOut(uint64_t position, int16_t* target, uint32_t frames) {
        uint32_t index = static_cast<uint32_t>(position % capacity_);
        uint32_t first = std::min(frames, capacity_ - index);
        std::memcpy(target, &samples_[static_cast<size_t>(index) * 2],
                    static_cast<size_t>(first) * 2 * sizeof(int16_t));
        if (first < frames) {
            std::memcpy(target + static_cast<size_t>(first) * 2, samples_.data(),
                        static_cast<size_t>(frames - first) * 2 * sizeof(int16_t));
        }
    }

    uint32_t capacity_;
    std::vector<int16_t> samples_;
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
};

class MonoPcmRingBuffer {
public:
    explicit MonoPcmRingBuffer(uint32_t capacityFrames)
        : capacity_(capacityFrames), samples_(capacityFrames) {
    }

    uint32_t capacity() const { return capacity_; }

    uint32_t available() const {
        uint64_t write = write_.load(std::memory_order_acquire);
        uint64_t read = read_.load(std::memory_order_acquire);
        return static_cast<uint32_t>(std::min<uint64_t>(write - read, capacity_));
    }

    uint32_t push(const int16_t* source, uint32_t frames) {
        uint64_t write = write_.load(std::memory_order_relaxed);
        uint64_t read = read_.load(std::memory_order_acquire);
        uint32_t freeFrames = capacity_ - static_cast<uint32_t>(write - read);
        uint32_t accepted = std::min(frames, freeFrames);
        copyIn(write, source, accepted);
        write_.store(write + accepted, std::memory_order_release);
        return accepted;
    }

    uint32_t pop(int16_t* target, uint32_t frames) {
        uint64_t read = read_.load(std::memory_order_relaxed);
        uint64_t write = write_.load(std::memory_order_acquire);
        uint32_t count = std::min<uint32_t>(
                frames, static_cast<uint32_t>(write - read));
        copyOut(read, target, count);
        read_.store(read + count, std::memory_order_release);
        return count;
    }

private:
    void copyIn(uint64_t position, const int16_t* source, uint32_t frames) {
        uint32_t index = static_cast<uint32_t>(position % capacity_);
        uint32_t first = std::min(frames, capacity_ - index);
        std::memcpy(&samples_[index], source, static_cast<size_t>(first) * sizeof(int16_t));
        if (first < frames) {
            std::memcpy(samples_.data(), source + first,
                        static_cast<size_t>(frames - first) * sizeof(int16_t));
        }
    }

    void copyOut(uint64_t position, int16_t* target, uint32_t frames) {
        uint32_t index = static_cast<uint32_t>(position % capacity_);
        uint32_t first = std::min(frames, capacity_ - index);
        std::memcpy(target, &samples_[index], static_cast<size_t>(first) * sizeof(int16_t));
        if (first < frames) {
            std::memcpy(target + first, samples_.data(),
                        static_cast<size_t>(frames - first) * sizeof(int16_t));
        }
    }

    uint32_t capacity_;
    std::vector<int16_t> samples_;
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
};

}  // namespace fastaudio
