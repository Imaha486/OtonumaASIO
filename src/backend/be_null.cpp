// be_null.cpp - 試験用の出力 (音は出さずに定周期で消費する)
// 環境変数 OTO_NULL_DUMP=ファイル名 で受け取ったデータをそのまま書き出す (ビット一致の確認用)
// 環境変数 OTO_NULL_BITS=16/24/32 でコンテナ形式を変えられる
#include "backend_util.h"
#include "../engine/util.h"
#include <cstdio>
#include <vector>
#include <cstdlib>

namespace {
class NullBackend : public BsBackend {
public:
    ~NullBackend() { stop(); close(); }
    int type() const override { return BS_OUT_NULL; }
    bool attach(const std::wstring&, void*, std::wstring&) override { return true; }
    void detach() override {}
    bool can_rate(double r, bool dsd, int dsdMode) override {
        if (dsd) return dsdMode != BS_DSD_NATIVE && r >= 2822400.0 && r <= 2822400.0 * 32;
        return r >= 8000 && r <= 1536000;
    }
    bool open(const BsOpenRequest& rq, BsEngineIO* io, BsFormat& fmt, std::wstring& err) override {
        double rate; bool dop;
        if (!bs_pcm_target(rq, rate, dop, err)) return false;
        int bits = 32;
        wchar_t env[16] = {0};
        if (GetEnvironmentVariableW(L"OTO_NULL_BITS", env, 16)) bits = _wtoi(env);
        if (bits != 16 && bits != 24 && bits != 32) bits = 32;
        if (dop && bits == 16) bits = 24;
        int valid = bits == 32 ? (dop ? 24 : 32) : bits;
        bs_set_fmt(fmt, rate, rq.channels, bits, valid, dop);
        fmt_ = fmt; io_ = io; s_ = *rq.s;
        chunk_ = bs_chunk_for_hz(rate, s_.pacerHz);
        buf_.assign((size_t)chunk_ * fmt.frame_bytes(), 0);
        wchar_t path[MAX_PATH] = {0};
        if (GetEnvironmentVariableW(L"OTO_NULL_DUMP", path, MAX_PATH)) dump_ = _wfopen(path, L"ab");
        return true;
    }
    bool start(std::wstring&) override {
        if (th_) return true;
        stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        th_ = CreateThread(nullptr, 0, s_run, this, 0, nullptr);
        return th_ != nullptr;
    }
    void stop() override {
        if (!th_) return;
        SetEvent(stopEv_);
        WaitForSingleObject(th_, INFINITE);
        CloseHandle(th_); th_ = nullptr;
        CloseHandle(stopEv_); stopEv_ = nullptr;
    }
    void close() override { if (dump_) { fclose(dump_); dump_ = nullptr; } }
    uint32_t chunk_frames() const override { return chunk_; }
    uint32_t latency_frames() const override { return chunk_; }
    double effective_hz() const override { return fmt_.deviceRate / (chunk_ ? chunk_ : 1); }
    std::wstring device_name() const override { return L"null（試験用）"; }
    std::wstring info() const override { return dump_ ? L"null 出力 (ファイルへ書き出し中)" : L"null 出力"; }

private:
    static DWORD WINAPI s_run(void* p) { ((NullBackend*)p)->run(); return 0; }
    void run() {
        io_->out_thread_begin();
        BsPacer pacer;
        pacer.start(chunk_ / fmt_.deviceRate, s_.driveMode == BS_DRIVE_TIMER_SPIN, s_.spinUs);
        while (pacer.wait(stopEv_)) {
            io_->wake_mark();
            io_->out_thread_poll();
            io_->pull(buf_.data(), chunk_);
            if (dump_) fwrite(buf_.data(), 1, buf_.size(), dump_);
        }
        io_->out_thread_end();
    }
    BsFormat fmt_;
    BsEngineIO* io_ = nullptr;
    BsSettings s_;
    uint32_t chunk_ = 4410;
    std::vector<uint8_t> buf_;
    HANDLE th_ = nullptr, stopEv_ = nullptr;
    FILE* dump_ = nullptr;
};

// ネットワーク出力: 既存 BareStream のクライアント部を差し込む場所 (段階8)
class NetBackend : public BsBackend {
public:
    int type() const override { return BS_OUT_NET; }
    bool attach(const std::wstring&, void*, std::wstring& err) override {
        err = L"BareStream ネットワーク出力は段階8で既存の BareStreamASIO と統合します (この版では未対応)";
        return false;
    }
    void detach() override {}
    bool can_rate(double, bool, int) override { return false; }
    bool open(const BsOpenRequest&, BsEngineIO*, BsFormat&, std::wstring& err) override { err = L"未対応"; return false; }
    bool start(std::wstring&) override { return false; }
    void stop() override {}
    void close() override {}
    uint32_t chunk_frames() const override { return 0; }
    uint32_t latency_frames() const override { return 0; }
    double effective_hz() const override { return 0; }
    std::wstring device_name() const override { return L"BareStream ネットワーク"; }
};
} // namespace

BsBackend* bs_new_null() { return new NullBackend(); }
BsBackend* bs_new_net() { return new NetBackend(); }
