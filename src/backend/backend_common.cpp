#include "backend.h"
#include "backend_util.h"

BsBackend* bs_create_backend(int outType) {
    switch (outType) {
    case BS_OUT_ASIO: return bs_new_asio();
    case BS_OUT_WASAPI: return bs_new_wasapi();
    case BS_OUT_KS_AUTO: case BS_OUT_KS_PACKET: case BS_OUT_KS_RT: return bs_new_ks(outType);
    case BS_OUT_NET: return bs_new_net();
    case BS_OUT_NULL: return bs_new_null();
    }
    return nullptr;
}

void bs_enum_devices(int outType, std::vector<BsDeviceInfo>& out) {
    out.clear();
    switch (outType) {
    case BS_OUT_ASIO: bs_enum_asio(out); break;
    case BS_OUT_WASAPI: bs_enum_wasapi(out); break;
    case BS_OUT_KS_AUTO: case BS_OUT_KS_PACKET: case BS_OUT_KS_RT: bs_enum_ks(out); break;
    case BS_OUT_NULL: out.push_back(BsDeviceInfo{ L"null", L"null（音は出さない・試験用）" }); break;
    case BS_OUT_NET: out.push_back(BsDeviceInfo{ L"", L"（段階8で既存BareStreamと統合）" }); break;
    }
}

bool bs_pcm_target(const BsOpenRequest& rq, double& devRate, bool& dop, std::wstring& err) {
    dop = false;
    devRate = rq.hostRate;
    if (rq.dsd) {
        if (rq.dsdMode == BS_DSD_NATIVE) {
            err = L"この出力方式はネイティブ DSD に対応していません (DoP を使うか ASIO 中継にしてください)";
            return false;
        }
        if (rq.s && rq.hostRate > bs_dop_max_rate(rq.s->dopMax) + 1.0) {
            err = L"DoP で送れる上限を超えた DSD レートです (設定の「DoP 上限」を確認するか、ネイティブ DSD に対応した ASIO 中継を使ってください)";
            return false;
        }
        dop = true;
        devRate = rq.hostRate / 16.0;
    }
    return true;
}

std::vector<BsPcmCandidate> bs_pcm_candidates(bool dop) {
    std::vector<BsPcmCandidate> v;
    // 32/32 → 24/32 → 24/24 → 16/16 の順
    if (!dop) v.push_back(BsPcmCandidate{ 32, 32 });
    v.push_back(BsPcmCandidate{ 32, 24 });
    v.push_back(BsPcmCandidate{ 24, 24 });
    if (!dop) v.push_back(BsPcmCandidate{ 16, 16 });
    return v;
}

void bs_fill_wfx(BsWaveFormatExtensible& w, double rate, int channels, int containerBits, int validBits) {
    memset(&w, 0, sizeof w);
    w.Format.wFormatTag = 0xFFFE;  // WAVE_FORMAT_EXTENSIBLE
    w.Format.nChannels = (WORD)channels;
    w.Format.nSamplesPerSec = (DWORD)(rate + 0.5);
    w.Format.wBitsPerSample = (WORD)containerBits;
    w.Format.nBlockAlign = (WORD)(channels * containerBits / 8);
    w.Format.nAvgBytesPerSec = w.Format.nSamplesPerSec * w.Format.nBlockAlign;
    w.Format.cbSize = 22;
    w.wValidBitsPerSample = (WORD)validBits;
    w.dwChannelMask = channels == 2 ? 0x3 : (channels == 1 ? 0x4 : 0);
    w.SubFormat = BS_KSDATAFORMAT_SUBTYPE_PCM;
}
