#include "NvencEncoder.h"

#include <chrono>
#include <cstdio>
#include <string>

#include <d3d11.h>
#include <d3d11_4.h>

NvEncoderD3D11Base::NvEncoderD3D11Base(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                                       uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrateKbps)
    : m_dev(dev)
    , m_ctx(ctx)
    , m_w(w)
    , m_h(h)
    , m_fps(fps)
    , m_bitrate(bitrateKbps)
{
    ZeroMemory(&m_fn, sizeof(m_fn));
    m_fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
}

NvEncoderD3D11Base::~NvEncoderD3D11Base()
{
    m_texReg.clear();

    if (m_hEncoder && m_fn.nvEncDestroyEncoder) {
        m_fn.nvEncDestroyEncoder(m_hEncoder);
        m_hEncoder = nullptr;
    }
}

bool NvEncoderD3D11Base::LoadApi()
{
    NVENCSTATUS status = NvEncodeAPICreateInstance(&m_fn);
    if (status != NV_ENC_SUCCESS) {
        Log("NvEncodeAPICreateInstance failed");
        return false;
    }
    return true;
}

bool NvEncoderD3D11Base::OpenSession()
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    params.device = m_dev;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS st = m_fn.nvEncOpenEncodeSessionEx(&params, &m_hEncoder);
    if (st != NV_ENC_SUCCESS) {
        char buf[256];
        sprintf_s(buf, "nvEncOpenEncodeSessionEx failed: %d", (int)st);
        Log(buf);
        return false;
    }
    return true;
}

bool NvEncoderD3D11Base::InitEncoder(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrateKbps)
{
    NV_ENC_PRESET_CONFIG presetCfg = { NV_ENC_PRESET_CONFIG_VER };
    presetCfg.presetCfg.version = NV_ENC_CONFIG_VER;

    NVENCSTATUS st = m_fn.nvEncGetEncodePresetConfigEx(
        m_hEncoder,
        CodecGuid(),
        NV_ENC_PRESET_P1_GUID,
        NV_ENC_TUNING_INFO_LOW_LATENCY,
        &presetCfg);
    if (st != NV_ENC_SUCCESS) {
        Log("nvEncGetEncodePresetConfigEx failed");
        return false;
    }

    NV_ENC_CONFIG cfg = presetCfg.presetCfg;

    cfg.gopLength = fps;
    cfg.frameIntervalP = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    cfg.rcParams.averageBitRate  = bitrateKbps * 1000;
    cfg.rcParams.maxBitRate      = bitrateKbps * 1000;
    cfg.rcParams.vbvBufferSize   = bitrateKbps * 1000;
    cfg.rcParams.vbvInitialDelay = bitrateKbps * 500;

    ConfigureCodec(cfg, fps, bitrateKbps);

    NV_ENC_INITIALIZE_PARAMS init = { NV_ENC_INITIALIZE_PARAMS_VER };
    init.encodeGUID = CodecGuid();
    init.presetGUID = NV_ENC_PRESET_P1_GUID;
    init.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init.encodeWidth  = w;
    init.encodeHeight = h;
    init.darWidth     = w;
    init.darHeight    = h;
    init.frameRateNum = fps;
    init.frameRateDen = 1;
    init.enablePTD    = 1;
    init.encodeConfig = &cfg;

    st = m_fn.nvEncInitializeEncoder(m_hEncoder, &init);
    if (st != NV_ENC_SUCCESS) {
        Log("nvEncInitializeEncoder failed");
        return false;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER cbb = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    st = m_fn.nvEncCreateBitstreamBuffer(m_hEncoder, &cbb);
    if (st != NV_ENC_SUCCESS) {
        Log("nvEncCreateBitstreamBuffer failed");
        return false;
    }
    m_bsBuf = cbb.bitstreamBuffer;

    return true;
}

bool NvEncoderD3D11Base::Initialize()
{
    if (!LoadApi()) return false;
    if (!OpenSession()) return false;
    if (!InitEncoder(m_w, m_h, m_fps, m_bitrate)) return false;
    return true;
}

bool NvEncoderD3D11Base::EnsureTypedTexture(ID3D11Texture2D* src)
{
    if (!src) return false;

    D3D11_TEXTURE2D_DESC desc = {};
    src->GetDesc(&desc);

    if (desc.SampleDesc.Count != 1) {
        Log("MSAA textures are not supported");
        return false;
    }
    if (desc.ArraySize != 1) {
        Log("Texture arrays are not supported");
        return false;
    }
    if (desc.MipLevels != 1) {
        Log("Mipmapped textures are not supported");
        return false;
    }

    ID3D11Texture2D* srcTex = src;

    if (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
    {
        if (!m_typedTex || m_typedW != desc.Width || m_typedH != desc.Height)
        {
            char buf[256];
            sprintf_s(buf, "Tex desc: W=%u H=%u Format=%d SampleCount=%u ArraySize=%u MipLevels=%u",
                desc.Width, desc.Height, (int)desc.Format,
                desc.SampleDesc.Count, desc.ArraySize, desc.MipLevels);
            Log(buf);

            D3D11_TEXTURE2D_DESC tdesc = desc;
            tdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            tdesc.MipLevels = 1;
            tdesc.ArraySize = 1;
            tdesc.SampleDesc.Count = 1;
            tdesc.MiscFlags = 0;

            m_typedTex.Reset();
            HRESULT hr = m_dev->CreateTexture2D(&tdesc, nullptr, m_typedTex.GetAddressOf());
            if (FAILED(hr)) {
                Log("CreateTexture2D (typedTex) failed");
                return false;
            }
            m_typedW = desc.Width;
            m_typedH = desc.Height;

            Log("Created typed texture DXGI_FORMAT_R8G8B8A8_UNORM");
        }

        m_ctx->CopyResource(m_typedTex.Get(), src);
        srcTex = m_typedTex.Get();

        srcTex->GetDesc(&desc);
    }

    if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        m_bufFmt = NV_ENC_BUFFER_FORMAT_ABGR;
    }
    else if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
             desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
    {
        m_bufFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    }
    else {
        Log("Unsupported DXGI format even after typeless fix");
        return false;
    }

    return true;
}

bool NvEncoderD3D11Base::EncodeTexture(ID3D11Texture2D* tex, int64_t ts100ns,
                                       std::vector<std::vector<uint8_t>>& outPackets)
{
    outPackets.clear();
    if (!m_hEncoder) return false;
    if (!EnsureTypedTexture(tex)) return false;

    ID3D11Texture2D* srcTex = tex;
    if (m_typedTex)
        srcTex = m_typedTex.Get();

    D3D11_TEXTURE2D_DESC desc = {};
    srcTex->GetDesc(&desc);

    NV_ENC_REGISTERED_PTR reg = nullptr;
    auto it = m_texReg.find(srcTex);
    if (it == m_texReg.end()) {
        NV_ENC_REGISTER_RESOURCE rr = { NV_ENC_REGISTER_RESOURCE_VER };
        rr.resourceType       = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        rr.width              = desc.Width;
        rr.height             = desc.Height;
        rr.pitch              = 0;
        rr.subResourceIndex   = 0;
        rr.bufferFormat       = m_bufFmt;
        rr.bufferUsage        = NV_ENC_INPUT_IMAGE;
        rr.resourceToRegister = srcTex;

        NVENCSTATUS st = m_fn.nvEncRegisterResource(m_hEncoder, &rr);
        if (st != NV_ENC_SUCCESS) {
            char buf[256];
            sprintf_s(buf, "nvEncRegisterResource failed: %d", (int)st);
            Log(buf);
            return false;
        }
        reg = rr.registeredResource;
        m_texReg.emplace(srcTex, TexReg{reg});
    } else {
        reg = it->second.reg;
    }

    NV_ENC_MAP_INPUT_RESOURCE map = { NV_ENC_MAP_INPUT_RESOURCE_VER };
    map.registeredResource = reg;
    NVENCSTATUS st = m_fn.nvEncMapInputResource(m_hEncoder, &map);
    if (st != NV_ENC_SUCCESS) {
        Log("nvEncMapInputResource failed");
        return false;
    }

    NV_ENC_PIC_PARAMS pic = { NV_ENC_PIC_PARAMS_VER };
    pic.inputBuffer      = map.mappedResource;
    pic.bufferFmt        = m_bufFmt;
    pic.inputWidth       = desc.Width;
    pic.inputHeight      = desc.Height;
    pic.pictureStruct    = NV_ENC_PIC_STRUCT_FRAME;
    pic.outputBitstream  = m_bsBuf;
    pic.inputTimeStamp   = (uint64_t)ts100ns;
    if (m_firstFrame) {
        pic.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
        m_firstFrame = false;
    }

    st = m_fn.nvEncEncodePicture(m_hEncoder, &pic);
    if (st != NV_ENC_SUCCESS) {
        Log("nvEncEncodePicture failed");
        m_fn.nvEncUnmapInputResource(m_hEncoder, &map);
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
    lock.outputBitstream = m_bsBuf;
    lock.doNotWait = 0;
    st = m_fn.nvEncLockBitstream(m_hEncoder, &lock);
    if (st != NV_ENC_SUCCESS) {
        Log("nvEncLockBitstream failed");
        m_fn.nvEncUnmapInputResource(m_hEncoder, &map);
        return false;
    }

    uint8_t* ptr = (uint8_t*)lock.bitstreamBufferPtr;
    uint32_t sz  = lock.bitstreamSizeInBytes;

    if (sz > 0) {
        std::vector<uint8_t> buf(ptr, ptr + sz);
        outPackets.push_back(std::move(buf));
    }

    m_fn.nvEncUnlockBitstream(m_hEncoder, m_bsBuf);
    m_fn.nvEncUnmapInputResource(m_hEncoder, &map);

    return !outPackets.empty();
}

void NvEncoderD3D11Base::Flush(std::vector<std::vector<uint8_t>>& outPackets)
{
    outPackets.clear();
    if (!m_hEncoder) return;

    NV_ENC_PIC_PARAMS pic = { NV_ENC_PIC_PARAMS_VER };
    pic.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    pic.outputBitstream = m_bsBuf;
    NVENCSTATUS st = m_fn.nvEncEncodePicture(m_hEncoder, &pic);
    if (st != NV_ENC_SUCCESS) return;

    NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
    lock.outputBitstream = m_bsBuf;
    lock.doNotWait = 0;
    st = m_fn.nvEncLockBitstream(m_hEncoder, &lock);
    if (st != NV_ENC_SUCCESS) return;

    uint8_t* ptr = (uint8_t*)lock.bitstreamBufferPtr;
    uint32_t sz  = lock.bitstreamSizeInBytes;
    if (sz > 0) {
        std::vector<uint8_t> buf(ptr, ptr + sz);
        outPackets.push_back(std::move(buf));
    }

    m_fn.nvEncUnlockBitstream(m_hEncoder, m_bsBuf);
}
