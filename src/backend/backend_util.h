// backend_util.h - バックエンド共通の小物 (SDK ヘッダの差を避けるため自前定義)
#pragma once
#include "backend.h"
#include <cstring>

#pragma pack(push, 1)
struct BsWaveFormatEx {
    WORD  wFormatTag;
    WORD  nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
    WORD  wBitsPerSample;
    WORD  cbSize;
};
struct BsWaveFormatExtensible {
    BsWaveFormatEx Format;
    WORD  wValidBitsPerSample;
    DWORD dwChannelMask;
    GUID  SubFormat;
};
#pragma pack(pop)

static const GUID BS_KSDATAFORMAT_SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

struct BsPcmCandidate { int container; int valid; };

bool bs_pcm_target(const BsOpenRequest& rq, double& devRate, bool& dop, std::wstring& err);
std::vector<BsPcmCandidate> bs_pcm_candidates(bool dop);
void bs_fill_wfx(BsWaveFormatExtensible& w, double rate, int channels, int containerBits, int validBits);

inline void bs_set_fmt(BsFormat& f, double rate, int ch, int container, int valid, bool dop) {
    f.deviceRate = rate; f.channels = ch; f.containerBits = container; f.validBits = valid;
    f.kind = dop ? BS_KIND_DOP : BS_KIND_PCM;
}

inline std::wstring bs_hr_str(const wchar_t* what, long hr) {
    wchar_t b[64]; swprintf(b, 64, L" (0x%08lX)", (unsigned long)hr);
    return std::wstring(what) + b;
}
