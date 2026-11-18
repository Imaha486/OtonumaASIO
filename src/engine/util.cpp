#include "util.h"
#include <avrt.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static uint32_t g_crcTable[256];
static struct CrcInit {
    CrcInit() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            g_crcTable[i] = c;
        }
    }
} g_crcInit;

uint32_t bs_crc32_update(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    while (len--) crc = g_crcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

BsPacer::BsPacer() {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer_) highRes_ = true;
    else timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
}

BsPacer::~BsPacer() { if (timer_) CloseHandle(timer_); }

void BsPacer::start(double periodSec, bool spin, int spinUs) {
    period_ = (int64_t)(periodSec * (double)bs_qpf());
    if (period_ < 1) period_ = 1;
    set_spin(spin, spinUs);
    next_ = bs_qpc() + period_;
}

bool BsPacer::wait(HANDLE stopEvent) {
    int64_t now = bs_qpc();
    // 大きく遅れたら予定を今に合わせ直す (追いつこうとして連続起床しない)
    if (now - next_ > period_ * 2) next_ = now;
    int64_t wakeAt = next_ - (spin_ ? spinTicks_ : 0);
    if (wakeAt > now) {
        int64_t hns = (wakeAt - now) * 10000000LL / bs_qpf();
        if (hns > 0) {
            LARGE_INTEGER due; due.QuadPart = -hns;
            SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0);
            HANDLE hs[2] = { stopEvent, timer_ };
            DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0) return false;
        }
    } else if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
        return false;
    }
    if (spin_) {
        while (bs_qpc() < next_) YieldProcessor();
    }
    next_ += period_;
    return true;
}

void BsThreadTuning::apply(int prio, int core) {
    if (prio != appliedPrio) {
        if (avrt) { AvRevertMmThreadCharacteristics(avrt); avrt = nullptr; }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        if (prio == BS_PRIO_TIMECRITICAL) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        } else {
            DWORD idx = 0;
            avrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
            if (avrt) AvSetMmThreadPriority(avrt, prio == BS_PRIO_CRITICAL ? AVRT_PRIORITY_CRITICAL : AVRT_PRIORITY_NORMAL);
            else SetThreadPriority(GetCurrentThread(), prio == BS_PRIO_CRITICAL ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL);
        }
        appliedPrio = prio;
    }
    if (core != appliedCore) {
        if (core >= 0 && core < (int)(sizeof(DWORD_PTR) * 8)) {
            DWORD_PTR procMask = 0, sysMask = 0;
            GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);
            DWORD_PTR m = (DWORD_PTR)1 << core;
            if (procMask & m) SetThreadAffinityMask(GetCurrentThread(), m);
        } else {
            DWORD_PTR procMask = 0, sysMask = 0;
            if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) && procMask)
                SetThreadAffinityMask(GetCurrentThread(), procMask);
        }
        appliedCore = core;
    }
}

void BsThreadTuning::revert() {
    if (avrt) { AvRevertMmThreadCharacteristics(avrt); avrt = nullptr; }
    appliedPrio = -1; appliedCore = -2;
}
