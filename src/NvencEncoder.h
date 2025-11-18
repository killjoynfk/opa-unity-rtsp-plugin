#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <wrl/client.h>

#include <Windows.h>
#include "nvEncodeAPI.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "NvencRtspPlugin.h"

// Обёртка над NVENC для Direct3D11. Базовый класс реализует всю общую
// работу с NVENC, а конкретные кодеки переопределяют детали конфигурации.
class NvEncoderD3D11Base
{
public:
    NvEncoderD3D11Base(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                       uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrateKbps);
    virtual ~NvEncoderD3D11Base();

    bool Initialize();
    bool EncodeTexture(ID3D11Texture2D* tex, int64_t ts100ns,
                       std::vector<std::vector<uint8_t>>& outPackets);
    void Flush(std::vector<std::vector<uint8_t>>& outPackets);

    AVCodecID GetCodecId() const { return GetAvCodecId(); }
    bool PacketHasIdr(const uint8_t* p, size_t n) const { return PacketHasIdrImpl(p, n); }

protected:
    virtual GUID CodecGuid() const = 0;
    virtual void ConfigureCodec(NV_ENC_CONFIG& cfg, uint32_t fps, uint32_t bitrateKbps) = 0;
    virtual AVCodecID GetAvCodecId() const = 0;
    virtual bool PacketHasIdrImpl(const uint8_t* p, size_t n) const = 0;

private:
    bool LoadApi();
    bool OpenSession();
    bool InitEncoder(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrateKbps);
    bool EnsureTypedTexture(ID3D11Texture2D* src);

protected:
    ID3D11Device*        m_dev  = nullptr;
    ID3D11DeviceContext* m_ctx  = nullptr;

private:
    NV_ENCODE_API_FUNCTION_LIST m_fn = {};
    void* m_hEncoder = nullptr;

    NV_ENC_BUFFER_FORMAT m_bufFmt = NV_ENC_BUFFER_FORMAT_ABGR;

    struct TexReg {
        NV_ENC_REGISTERED_PTR reg = nullptr;
    };

    std::unordered_map<ID3D11Texture2D*, TexReg> m_texReg;

    NV_ENC_OUTPUT_PTR m_bsBuf = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_typedTex;
    uint32_t m_typedW = 0;
    uint32_t m_typedH = 0;

    bool m_firstFrame = true;

    uint32_t m_w = 0;
    uint32_t m_h = 0;
    uint32_t m_fps = 0;
    uint32_t m_bitrate = 0;
};

std::unique_ptr<NvEncoderD3D11Base> CreateNvEncoder(
    NvrtspCodec codec,
    ID3D11Device* dev,
    ID3D11DeviceContext* ctx,
    uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrateKbps);

// Объявление логгера из NvencRtspPlugin.cpp
void Log(const char* msg);
