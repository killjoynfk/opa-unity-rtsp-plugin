#pragma once

#include "NvencEncoder.h"

class NvEncoderD3D11_H265 : public NvEncoderD3D11Base
{
public:
    NvEncoderD3D11_H265(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                        uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrateKbps)
        : NvEncoderD3D11Base(dev, ctx, w, h, fps, bitrateKbps)
    {
    }

protected:
    GUID CodecGuid() const override;
    void ConfigureCodec(NV_ENC_CONFIG& cfg, uint32_t fps, uint32_t bitrateKbps) override;
    AVCodecID GetAvCodecId() const override;
    bool PacketHasIdrImpl(const uint8_t* p, size_t n) const override;
};
