#include "NvencEncoderH265.h"

GUID NvEncoderD3D11_H265::CodecGuid() const
{
    return NV_ENC_CODEC_HEVC_GUID;
}

void NvEncoderD3D11_H265::ConfigureCodec(NV_ENC_CONFIG& cfg, uint32_t fps, uint32_t bitrateKbps)
{
    cfg.encodeCodecConfig.hevcConfig.idrPeriod = fps;
    cfg.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
    cfg.encodeCodecConfig.hevcConfig.outputAUD   = 0;
    cfg.encodeCodecConfig.hevcConfig.disableSPSPPS = 0;
    cfg.encodeCodecConfig.hevcConfig.enableIntraRefresh = 0;
    cfg.encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB = 1;
    cfg.encodeCodecConfig.hevcConfig.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
}

AVCodecID NvEncoderD3D11_H265::GetAvCodecId() const
{
    return AV_CODEC_ID_HEVC;
}

bool NvEncoderD3D11_H265::PacketHasIdrImpl(const uint8_t* p, size_t n) const
{
    auto is_start = [&](size_t pos) -> bool {
        if (pos + 3 >= n) return false;
        if (p[pos] == 0 && p[pos+1] == 0 && p[pos+2] == 1)
            return true;
        if (pos + 4 <= n && p[pos] == 0 && p[pos+1] == 0 && p[pos+2] == 0 && p[pos+3] == 1)
            return true;
        return false;
    };

    size_t i = 0;
    while (i + 4 < n) {
        while (i + 4 < n && !is_start(i))
            ++i;
        if (!is_start(i)) break;

        size_t nalStart = (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1) ? i + 3 : i + 4;
        if (nalStart + 1 >= n)
            break;

        uint8_t nalType = (p[nalStart] & 0x7E) >> 1;
        if (nalType == 19 || nalType == 20)
            return true;

        i = nalStart + 1;
    }
    return false;
}
