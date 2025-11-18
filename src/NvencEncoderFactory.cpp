#include "NvencEncoder.h"
#include "NvencEncoderH264.h"
#include "NvencEncoderH265.h"

std::unique_ptr<NvEncoderD3D11Base> CreateNvEncoder(
    NvrtspCodec codec,
    ID3D11Device* dev,
    ID3D11DeviceContext* ctx,
    uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrateKbps)
{
    switch (codec)
    {
    case NVRTSP_CODEC_H264:
        return std::make_unique<NvEncoderD3D11_H264>(dev, ctx, w, h, fps, bitrateKbps);
    case NVRTSP_CODEC_H265:
        return std::make_unique<NvEncoderD3D11_H265>(dev, ctx, w, h, fps, bitrateKbps);
    default:
        return nullptr;
    }
}
