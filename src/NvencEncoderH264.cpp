#include "NvencEncoderH264.h"

GUID NvEncoderD3D11_H264::CodecGuid() const
{
    return NV_ENC_CODEC_H264_GUID;
}

void NvEncoderD3D11_H264::ConfigureCodec(NV_ENC_CONFIG& cfg, uint32_t fps, uint32_t bitrateKbps)
{
    cfg.encodeCodecConfig.h264Config.idrPeriod = fps;
    cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    cfg.encodeCodecConfig.h264Config.outputAUD   = 0;
    cfg.encodeCodecConfig.h264Config.disableSPSPPS = 0;
    cfg.encodeCodecConfig.h264Config.enableIntraRefresh = 0;
    cfg.encodeCodecConfig.h264Config.maxNumRefFrames = 1;
    cfg.encodeCodecConfig.h264Config.bdirectMode = NV_ENC_H264_BDIRECT_MODE_DISABLE;
    cfg.encodeCodecConfig.h264Config.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;
}

AVCodecID NvEncoderD3D11_H264::GetAvCodecId() const
{
    return AV_CODEC_ID_H264;
}

bool NvEncoderD3D11_H264::PacketHasIdrImpl(const uint8_t* p, size_t n) const
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

        size_t nalStart = 0;
        if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1)
            nalStart = i + 3;
        else
            nalStart = i + 4;

        if (nalStart >= n)
            break;

        uint8_t nal = p[nalStart] & 0x1F;
        if (nal == 5)
            return true;

        i = nalStart + 1;
    }
    return false;
}
