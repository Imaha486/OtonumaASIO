// ring.h - 書き手1・読み手1のロックフリー・リングバッファ (フレーム単位)
#pragma once
#include "../common/bs_common.h"
#include <atomic>
#include <cstring>

class BsRing {
public:
    ~BsRing() { free_mem(); }

    // capacityFrames × frameBytes を確保。lockMem なら VirtualLock で物理メモリに固定。
    bool alloc(uint64_t capacityFrames, uint32_t frameBytes, bool lockMem, bool* lockedOut) {
        free_mem();
        cap_ = capacityFrames; fb_ = frameBytes;
        bytes_ = (size_t)(cap_ * fb_);
        mem_ = (uint8_t*)VirtualAlloc(nullptr, bytes_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem_) return false;
        memset(mem_, 0, bytes_);   // 先に触ってページを確保しておく
        locked_ = false;
        if (lockMem) {
            SIZE_T mn = 0, mx = 0;
            if (GetProcessWorkingSetSize(GetCurrentProcess(), &mn, &mx)) {
                SIZE_T add = bytes_ + (16u << 20);
                SetProcessWorkingSetSize(GetCurrentProcess(), mn + add, (mx > mn + add ? mx : mn + add) + add);
            }
            locked_ = VirtualLock(mem_, bytes_) != 0;
        }
        if (lockedOut) *lockedOut = locked_;
        w_.store(0); r_.store(0);
        return true;
    }

    void free_mem() {
        if (mem_) {
            if (locked_) VirtualUnlock(mem_, bytes_);
            VirtualFree(mem_, 0, MEM_RELEASE);
            mem_ = nullptr;
        }
        cap_ = 0; locked_ = false;
    }

    uint64_t capacity() const { return cap_; }
    uint32_t frame_bytes() const { return fb_; }
    uint64_t fill() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }
    uint64_t space() const { return cap_ - fill(); }
    uint64_t write_pos() const { return w_.load(std::memory_order_acquire); }
    uint64_t read_pos() const { return r_.load(std::memory_order_acquire); }

    // 書き手専用。書けた量を返す。
    size_t write(const uint8_t* src, size_t frames) {
        uint64_t w = w_.load(std::memory_order_relaxed);
        uint64_t r = r_.load(std::memory_order_acquire);
        uint64_t sp = cap_ - (w - r);
        if (frames > sp) frames = (size_t)sp;
        copy_in(w, src, frames);
        w_.store(w + frames, std::memory_order_release);
        return frames;
    }
    // 書き手専用: 直接書き込める領域 (最大2分割) を得る
    uint8_t* write_ptr(size_t& contiguousFrames) {
        uint64_t w = w_.load(std::memory_order_relaxed);
        uint64_t r = r_.load(std::memory_order_acquire);
        uint64_t sp = cap_ - (w - r);
        uint64_t off = w % cap_;
        uint64_t till = cap_ - off;
        contiguousFrames = (size_t)(sp < till ? sp : till);
        return mem_ + off * fb_;
    }
    void commit_write(size_t frames) { w_.store(w_.load(std::memory_order_relaxed) + frames, std::memory_order_release); }

    // 読み手専用
    size_t read(uint8_t* dst, size_t frames) {
        uint64_t r = r_.load(std::memory_order_relaxed);
        uint64_t w = w_.load(std::memory_order_acquire);
        uint64_t av = w - r;
        if (frames > av) frames = (size_t)av;
        uint64_t off = r % cap_;
        uint64_t first = cap_ - off; if (first > frames) first = frames;
        memcpy(dst, mem_ + off * fb_, (size_t)(first * fb_));
        if (frames > first) memcpy(dst + first * fb_, mem_, (size_t)((frames - first) * fb_));
        r_.store(r + frames, std::memory_order_release);
        return frames;
    }
    // 読み手専用: 中身を捨てる
    void drop_all() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }
    // 双方が止まっているときだけ
    void reset() { w_.store(0); r_.store(0); }

private:
    void copy_in(uint64_t w, const uint8_t* src, size_t frames) {
        uint64_t off = w % cap_;
        uint64_t first = cap_ - off; if (first > frames) first = frames;
        memcpy(mem_ + off * fb_, src, (size_t)(first * fb_));
        if (frames > first) memcpy(mem_, src + first * fb_, (size_t)((frames - first) * fb_));
    }
    uint8_t* mem_ = nullptr;
    size_t bytes_ = 0;
    uint64_t cap_ = 0;
    uint32_t fb_ = 1;
    bool locked_ = false;
    std::atomic<uint64_t> w_{0};
    char pad_[64];
    std::atomic<uint64_t> r_{0};
};
