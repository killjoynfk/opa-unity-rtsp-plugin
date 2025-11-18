#include "NvencRtspPlugin.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>        // для ID3D11Multithread
#include <wrl/client.h>

#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"

#include "nvEncodeAPI.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
}

// -----------------------------------------------------------------------------
// Глобалы Unity / D3D11
// -----------------------------------------------------------------------------

static IUnityInterfaces*                    g_unity         = nullptr;
static IUnityGraphics*                      g_ugraphics     = nullptr;
static IUnityGraphicsD3D11*                 g_ugraphicsD3D11= nullptr;
static Microsoft::WRL::ComPtr<ID3D11Device>        g_device;
static Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_context;
static Microsoft::WRL::ComPtr<ID3D11Multithread>   g_multithread;

static NvrtspLogCallback g_logCb = nullptr;

static void Log(const char* msg)
{
    if (g_logCb)
        g_logCb(msg);
#ifdef _DEBUG
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
#endif
}

static const char* NvStatusToStr(NVENCSTATUS st)
{
    switch (st)
    {
    case NV_ENC_SUCCESS: return "NV_ENC_SUCCESS";
    case NV_ENC_ERR_INVALID_PTR: return "NV_ENC_ERR_INVALID_PTR";
    case NV_ENC_ERR_INVALID_PARAM: return "NV_ENC_ERR_INVALID_PARAM";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "NV_ENC_ERR_UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_UNIMPLEMENTED: return "NV_ENC_ERR_UNIMPLEMENTED";
    case NV_ENC_ERR_OUT_OF_MEMORY: return "NV_ENC_ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_GENERIC: return "NV_ENC_ERR_GENERIC";
    default: return "NV_ENC_ERR_<unknown>";
    }
}

// -----------------------------------------------------------------------------
// NVENC D3D11 обёртка
// -----------------------------------------------------------------------------

class NvEncoderD3D11_V12
{
public:
    NvEncoderD3D11_V12(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                       uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrateKbps);
    ~NvEncoderD3D11_V12();

    bool Initialize();
    bool EncodeTexture(ID3D11Texture2D* tex, int64_t ts100ns,
                       std::vector<std::vector<uint8_t>>& outPackets);
    void Flush(std::vector<std::vector<uint8_t>>& outPackets);

private:
    bool LoadApi();
    bool OpenSession();
    bool InitEncoder(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrateKbps);
    bool EnsureTypedTexture(ID3D11Texture2D* src);

private:
    ID3D11Device*        m_dev  = nullptr;
    ID3D11DeviceContext* m_ctx  = nullptr;

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

NvEncoderD3D11_V12::NvEncoderD3D11_V12(ID3D11Device* dev, ID3D11DeviceContext* ctx,
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

NvEncoderD3D11_V12::~NvEncoderD3D11_V12()
{
    m_texReg.clear();

    if (m_hEncoder && m_fn.nvEncDestroyEncoder) {
        m_fn.nvEncDestroyEncoder(m_hEncoder);
        m_hEncoder = nullptr;
    }
}

bool NvEncoderD3D11_V12::LoadApi()
{
    NVENCSTATUS status = NvEncodeAPICreateInstance(&m_fn);
    if (status != NV_ENC_SUCCESS) {
        Log("NvEncodeAPICreateInstance failed");
        return false;
    }
    return true;
}

bool NvEncoderD3D11_V12::OpenSession()
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    params.device = m_dev;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS st = m_fn.nvEncOpenEncodeSessionEx(&params, &m_hEncoder);
    if (st != NV_ENC_SUCCESS) {
        char buf[256];
        sprintf_s(buf, "nvEncOpenEncodeSessionEx failed: %d (%s)", (int)st, NvStatusToStr(st));
        Log(buf);
        return false;
    }
    return true;
}

bool NvEncoderD3D11_V12::InitEncoder(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrateKbps)
{
    NV_ENC_PRESET_CONFIG presetCfg = { NV_ENC_PRESET_CONFIG_VER };
    presetCfg.presetCfg.version = NV_ENC_CONFIG_VER;

    NVENCSTATUS st = m_fn.nvEncGetEncodePresetConfigEx(
        m_hEncoder,
        NV_ENC_CODEC_H264_GUID,
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
    cfg.encodeCodecConfig.h264Config.idrPeriod = fps;
    cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    cfg.encodeCodecConfig.h264Config.outputAUD   = 0;
    cfg.encodeCodecConfig.h264Config.disableSPSPPS = 0;
    cfg.encodeCodecConfig.h264Config.enableIntraRefresh = 0;
    cfg.encodeCodecConfig.h264Config.maxNumRefFrames = 1;
    cfg.encodeCodecConfig.h264Config.bdirectMode = NV_ENC_H264_BDIRECT_MODE_DISABLE;
    cfg.encodeCodecConfig.h264Config.useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_DISABLED;


    NV_ENC_INITIALIZE_PARAMS init = { NV_ENC_INITIALIZE_PARAMS_VER };
    init.encodeGUID = NV_ENC_CODEC_H264_GUID;
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

bool NvEncoderD3D11_V12::Initialize()
{
    if (!LoadApi()) return false;
    if (!OpenSession()) return false;
    if (!InitEncoder(m_w, m_h, m_fps, m_bitrate)) return false;
    return true;
}

bool NvEncoderD3D11_V12::EnsureTypedTexture(ID3D11Texture2D* src)
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

static bool packet_has_idr(const uint8_t* p, size_t n)
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

bool NvEncoderD3D11_V12::EncodeTexture(ID3D11Texture2D* tex, int64_t ts100ns,
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
            sprintf_s(buf, "nvEncRegisterResource failed: %d (%s)", (int)st, NvStatusToStr(st));
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

void NvEncoderD3D11_V12::Flush(std::vector<std::vector<uint8_t>>& outPackets)
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

// -----------------------------------------------------------------------------
// FFmpeg / RTSP, состояние одного стрима (handle)
// -----------------------------------------------------------------------------

struct RtspState
{
    std::mutex mx;
    std::atomic<bool> running{false};
    std::thread worker;

    ID3D11Texture2D* srcTex = nullptr;
    uint32_t w = 0, h = 0, fps = 30, bitrate = 4000;

    std::wstring rtspUrlW;

    std::unique_ptr<NvEncoderD3D11_V12> nvenc;

    AVFormatContext* oc = nullptr;
    AVStream*        vst = nullptr;
    bool headerWritten = false;
};

static AVRational g_tb = {1, 90000};

static void close_rtsp_locked(RtspState& s)
{
    if (s.oc) {
        if (s.headerWritten) {
            av_write_trailer(s.oc);
        }
        avformat_free_context(s.oc);
    }
    s.oc = nullptr;
    s.vst = nullptr;
    s.headerWritten = false;
}

static bool open_rtsp_output_locked(RtspState& s)
{
    close_rtsp_locked(s);

    char url[1024] = {};
    std::wcstombs(url, s.rtspUrlW.c_str(), sizeof(url) - 1);

    avformat_network_init();

    const AVOutputFormat* ofmt = av_guess_format("rtsp", nullptr, nullptr);
    if (!ofmt) {
        Log("av_guess_format(rtsp) failed");
        return false;
    }

    if (avformat_alloc_output_context2(&s.oc, ofmt, "rtsp", url) < 0) {
        Log("avformat_alloc_output_context2 failed");
        return false;
    }

    av_opt_set(s.oc->priv_data, "rtsp_transport", "tcp", 0);
    av_opt_set(s.oc->priv_data, "muxdelay",      "0",   0);
    av_opt_set(s.oc->priv_data, "muxpreload",    "0",   0);

    s.vst = avformat_new_stream(s.oc, nullptr);
    if (!s.vst) {
        Log("avformat_new_stream failed");
        close_rtsp_locked(s);
        return false;
    }

    s.vst->id = 0;
    s.vst->time_base = g_tb;
    s.vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    s.vst->codecpar->codec_id   = AV_CODEC_ID_H264;
    s.vst->codecpar->format     = AV_PIX_FMT_YUV420P;
    s.vst->codecpar->width      = s.w;
    s.vst->codecpar->height     = s.h;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp",     0);
    av_dict_set(&opts, "muxdelay",       "0",       0);
    av_dict_set(&opts, "muxpreload",     "0",       0);
    av_dict_set(&opts, "stimeout",       "2000000", 0); // 2s
    av_dict_set(&opts, "timeout",        "2000000", 0);

    int ret = avformat_write_header(s.oc, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        char err[256];
        av_strerror(ret, err, sizeof(err));
        Log(err);
        close_rtsp_locked(s);
        return false;
    }

    s.headerWritten = true;
    Log("RTSP: avformat_write_header OK");
    return true;
}

static bool send_annexb_packet_locked(RtspState& s,
                                      const uint8_t* data, size_t len, int64_t ts100ns)
{
    if (!s.oc || !s.vst || !s.headerWritten)
        return false;

    bool keyframe = packet_has_idr(data, len);

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = const_cast<uint8_t*>(data);
    pkt.size = (int)len;
    pkt.stream_index = s.vst->index;

    if (ts100ns > 0) {
        int64_t pts90k = (ts100ns * 9) / 1000;
        pkt.pts = pkt.dts = pts90k;
    }

    if (keyframe)
        pkt.flags |= AV_PKT_FLAG_KEY;

    int ret = av_interleaved_write_frame(s.oc, &pkt);
    if (ret < 0) {
        char err[256];
        av_strerror(ret, err, sizeof(err));
        Log(err);
        return false;
    }

    return true;
}

// worker одного handle
static void rtsp_worker_thread(RtspState* s)
{
    Log("RTSP worker thread started");
    const double frameDurMs = 1000.0 / (s->fps ? s->fps : 30);
    auto nextTime = std::chrono::steady_clock::now();

    bool rtspReady = false;

    while (s->running) {
        nextTime += std::chrono::milliseconds((int)frameDurMs);

        // --- минимальный критический участок: просто читаем состояние ---
        ID3D11Texture2D* tex = nullptr;
        std::unique_ptr<NvEncoderD3D11_V12>* encPtr = nullptr;
        {
            std::lock_guard<std::mutex> lk(s->mx);

            if (!s->running)
                break;

            tex = s->srcTex;
            encPtr = &s->nvenc;
        }

        if (!tex || !encPtr || !encPtr->get()) {
            Log("RTSP worker: no srcTex or nvenc, exiting");
            s->running = false;
            break;
        }

        NvEncoderD3D11_V12* enc = encPtr->get();

        // --- открытие RTSP без мьютекса ---
        if (!rtspReady) {
            Log("RTSP worker: trying to open RTSP output...");
            if (open_rtsp_output_locked(*s)) {
                rtspReady = true;
                Log("RTSP worker: RTSP opened");
            }
            else {
                Log("RTSP worker: open_rtsp_output failed, will retry...");
                std::this_thread::sleep_until(nextTime);
                if (!s->running)
                    break;
                continue; // следующий тик, новая попытка
            }
        }

        // --- Encode + send, тоже без мьютекса ---
        int64_t ts100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count() / 100;

        std::vector<std::vector<uint8_t>> packets;
        if (enc->EncodeTexture(tex, ts100ns, packets)) {
            for (auto& p : packets) {
                if (!send_annexb_packet_locked(*s, p.data(), p.size(), ts100ns)) {
                    Log("RTSP worker: send_annexb_packet failed, closing RTSP and retrying later");
                    close_rtsp_locked(*s);
                    rtspReady = false;
                    break;
                }
            }
        }

        std::this_thread::sleep_until(nextTime);
        if (!s->running)
            break;
    }

    {
        std::lock_guard<std::mutex> lk(s->mx);
        close_rtsp_locked(*s);
    }

    Log("RTSP worker thread finished");
}


// -----------------------------------------------------------------------------
// Unity plugin entrypoints
// -----------------------------------------------------------------------------

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
    g_unity = unityInterfaces;
    g_ugraphics = g_unity->Get<IUnityGraphics>();
    g_ugraphicsD3D11 = g_unity->Get<IUnityGraphicsD3D11>();

    if (!g_ugraphicsD3D11) {
        Log("IUnityGraphicsD3D11 not available");
        return;
    }

    ID3D11Device* dev = g_ugraphicsD3D11->GetDevice();
    if (!dev) {
        Log("GetDevice returned null");
        return;
    }

    dev->QueryInterface(IID_PPV_ARGS(g_device.GetAddressOf()));
    if (!g_device) {
        Log("QueryInterface(ID3D11Device) failed");
        return;
    }

    g_device->GetImmediateContext(g_context.GetAddressOf());
    if (!g_context) {
        Log("GetImmediateContext failed");
        return;
    }

    ID3D11Multithread* mtRaw = nullptr;
    if (SUCCEEDED(g_context->QueryInterface(__uuidof(ID3D11Multithread),
                                            (void**)&mtRaw)))
    {
        g_multithread.Attach(mtRaw);
        g_multithread->SetMultithreadProtected(TRUE);
        Log("D3D11 multithread protection enabled");
    }

    Log("UnityPluginLoad OK");
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginUnload()
{
}

// -----------------------------------------------------------------------------
// C API на базе handle
// -----------------------------------------------------------------------------

NVRTSP_EXPORT void NVRTSP_SetLogCallback(NvrtspLogCallback cb)
{
    g_logCb = cb;
}

NVRTSP_EXPORT NvrtspHandle NVRTSP_Create(
    void* texPtr,
    int width, int height, int fps,
    int bitrateKbps,
    const wchar_t* rtspUrl)
{
    if (!g_device || !g_context) {
        Log("NVRTSP_Create: no D3D11 device/context");
        return nullptr;
    }
    if (!texPtr) {
        Log("NVRTSP_Create: null texture pointer");
        return nullptr;
    }

    RtspState* s = new RtspState();
    s->srcTex  = (ID3D11Texture2D*)texPtr;
    s->w       = (uint32_t)width;
    s->h       = (uint32_t)height;
    s->fps     = (uint32_t)fps;
    s->bitrate = (uint32_t)bitrateKbps;
    if (rtspUrl)
        s->rtspUrlW = rtspUrl;
    else
        s->rtspUrlW = L"";

    s->nvenc = std::make_unique<NvEncoderD3D11_V12>(
        g_device.Get(), g_context.Get(),
        s->w, s->h, s->fps, s->bitrate
    );
    if (!s->nvenc->Initialize()) {
        Log("NVRTSP_Create: NvEncoder init failed");
        delete s;
        return nullptr;
    }

    Log("NVRTSP_Create OK");
    return (NvrtspHandle)s;
}

NVRTSP_EXPORT bool NVRTSP_Start(NvrtspHandle handle)
{
    if (!handle)
        return false;

    RtspState* s = (RtspState*)handle;
    std::lock_guard<std::mutex> lk(s->mx);

    if (s->running) {
        Log("NVRTSP_Start: already running");
        return false;
    }

    s->running = true;
    s->worker = std::thread(rtsp_worker_thread, s);

    Log("NVRTSP_Start OK");
    return true;
}

NVRTSP_EXPORT void NVRTSP_Stop(NvrtspHandle handle)
{
    if (!handle)
        return;

    RtspState* s = (RtspState*)handle;

    // 1) Атомарно выключаем running без мьютекса
    bool expected = true;
    if (!s->running.compare_exchange_strong(expected, false)) {
        // уже остановлен
        return;
    }

    // 2) Ждём worker
    if (s->worker.joinable())
        s->worker.join();

    // 3) Теперь worker гарантированно не трогает s,
    //    можно спокойно чистить под мьютексом
    std::lock_guard<std::mutex> lk(s->mx);

    if (s->nvenc) {
        std::vector<std::vector<uint8_t>> tail;
        s->nvenc->Flush(tail);
        for (auto& p : tail) {
            send_annexb_packet_locked(*s, p.data(), p.size(), 0);
        }
        s->nvenc.reset();
    }

    close_rtsp_locked(*s);
    s->srcTex = nullptr;

    Log("NVRTSP_Stop done");
}


NVRTSP_EXPORT void NVRTSP_Destroy(NvrtspHandle handle)
{
    if (!handle)
        return;

    RtspState* s = (RtspState*)handle;

    NVRTSP_Stop(handle);

    delete s;
    Log("NVRTSP_Destroy done");
}
