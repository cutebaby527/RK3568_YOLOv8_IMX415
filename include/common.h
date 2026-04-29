#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#define SHM_NAME "/edge_ai_frame"
#define SHM_WIDTH 640
#define SHM_HEIGHT 480

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480
#define FRAME_NV12_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 3 / 2)

struct SharedDetect {
    char name[16];
    float prop;
    int left, top, right, bottom;
};

struct SharedFrame {
    std::atomic<uint64_t> seq{0};

    // NV12: Y plane + interleaved UV plane
    uint8_t nv12[FRAME_NV12_SIZE];

    SharedDetect detects[64];
    int detect_count{0};
};

struct RawFrame {
    uint8_t data[FRAME_NV12_SIZE];
    int width = FRAME_WIDTH;
    int height = FRAME_HEIGHT;
    uint64_t seq = 0;
};

struct DetectResult {
    char name[16];
    float prop;
    int left, top, right, bottom;
};

struct InferResult {
    DetectResult results[64];
    int count = 0;
    uint64_t seq = 0;
};

template <typename T, int N>
class RingQueue {
public:
    bool push(const T& item) {
        int next = (write_ + 1) % N;
        if (next == read_.load(std::memory_order_acquire)) return false;
        buf_[write_] = item;
        write_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        int r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        item = buf_[r];
        read_.store((r + 1) % N, std::memory_order_release);
        return true;
    }

private:
    T buf_[N];
    std::atomic<int> read_{0}, write_{0};
};
