// util.h - 時刻・CRC・ロックフリー小物
#pragma once
#include "../common/bs_common.h"
#include <atomic>
#include <stdint.h>

// ---- QPC ----
inline int64_t bs_qpc() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
inline int64_t bs_qpf() {
    static int64_t f = 0;
    if (!f) { LARGE_INTEGER v; QueryPerformanceFrequency(&v); f = v.QuadPart; }
    return f;
}
inline double bs_qpc_to_us(int64_t d) { return (double)d * 1e6 / (double)bs_qpf(); }

// ---- CRC32 (IEEE) ----
uint32_t bs_crc32_update(uint32_t crc, const void* data, size_t len);

// ---- 単一生産者・単一消費者の固定長キュー (値型) ----
template <class T, size_t N>
class SpscQueue {
public:
    bool push(const T& v) {
        size_t w = w_.load(std::memory_order_relaxed);
        size_t r = r_.load(std::memory_order_acquire);
        if (w - r >= N) return false;
        buf_[w % N] = v;
        w_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& v) {
        size_t r = r_.load(std::memory_order_relaxed);
        size_t w = w_.load(std::memory_order_acquire);
        if (r == w) return false;
        v = buf_[r % N];
        r_.store(r + 1, std::memory_order_release);
        return true;
    }
    void clear_from_consumer() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }
private:
    T buf_[N];
    std::atomic<size_t> w_{0}, r_{0};
};

// ---- 1 秒ごとの CRC 窓 (パイプライン内ビット一致検査) ----
struct CrcWindowItem { uint64_t index; uint32_t crc; };

class CrcWindow {
public:
    void reset(uint64_t windowFrames, uint32_t frameBytes) {
        win_ = windowFrames ? windowFrames : 1; fb_ = frameBytes; pos_ = 0; crc_ = 0; idx_ = 0;
        q.clear_from_consumer();
    }
    // frames 分のデータを窓に流す。窓が埋まるたびにキューへ積む。
    void feed(const uint8_t* p, size_t frames) {
        while (frames) {
            uint64_t room = win_ - pos_;
            size_t n = (size_t)(frames < room ? frames : room);
            crc_ = bs_crc32_update(crc_, p, n * fb_);
            p += n * fb_; frames -= n; pos_ += n;
            if (pos_ == win_) {
                CrcWindowItem it{ idx_, crc_ };
                q.push(it);
                idx_++; pos_ = 0; crc_ = 0;
            }
        }
    }
    SpscQueue<CrcWindowItem, 64> q;
private:
    uint64_t win_ = 1, pos_ = 0, idx_ = 0;
    uint32_t fb_ = 1, crc_ = 0;
};

// ---- 定周期待ち (タイマー / タイマー＋スピン) ----
class BsPacer {
public:
    BsPacer();
    ~BsPacer();
    void start(double periodSec, bool spin, int spinUs);
    // 次の周期まで待つ。stopEvent がシグナルされたら false。
    bool wait(HANDLE stopEvent);
    void set_spin(bool spin, int spinUs) { spin_ = spin; spinTicks_ = (int64_t)((double)spinUs * bs_qpf() / 1e6); }
private:
    HANDLE timer_ = nullptr;
    bool highRes_ = false;
    bool spin_ = false;
    int64_t period_ = 0, next_ = 0, spinTicks_ = 0;
};

// ノーマライズしたスレッド優先度などの適用 (出力スレッド用)
struct BsThreadTuning {
    HANDLE avrt = nullptr;
    int appliedPrio = -1;
    int appliedCore = -2;
    void apply(int prio, int core);
    void revert();
};
