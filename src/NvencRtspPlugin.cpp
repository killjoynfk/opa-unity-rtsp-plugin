#include "NvencRtspPlugin.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <memory>

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>        // для ID3D11Multithread
#include <wrl/client.h>

#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"

#include "NvencEncoder.h"

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

void Log(const char* msg)
{
    if (g_logCb)
        g_logCb(msg);
#ifdef _DEBUG
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
#endif
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

    NvrtspCodec codec = NVRTSP_CODEC_H264;

    std::wstring rtspUrlW;

    std::unique_ptr<NvEncoderD3D11Base> encoder;

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

    AVCodecID codecId = s.encoder ? s.encoder->GetCodecId() : AV_CODEC_ID_NONE;
    if (codecId == AV_CODEC_ID_NONE) {
        Log("RTSP: no encoder for stream");
        close_rtsp_locked(s);
        return false;
    }

    s.vst->id = 0;
    s.vst->time_base = g_tb;
    s.vst->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    s.vst->codecpar->codec_id   = codecId;
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

    bool keyframe = s.encoder ? s.encoder->PacketHasIdr(data, len) : false;

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
        std::unique_ptr<NvEncoderD3D11Base>* encPtr = nullptr;
        {
            std::lock_guard<std::mutex> lk(s->mx);

            if (!s->running)
                break;

            tex = s->srcTex;
            encPtr = &s->encoder;
        }

        if (!tex || !encPtr || !encPtr->get()) {
            Log("RTSP worker: no srcTex or encoder, exiting");
            s->running = false;
            break;
        }

        NvEncoderD3D11Base* enc = encPtr->get();

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
    NvrtspCodec codec,
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
    s->codec   = codec;
    if (rtspUrl)
        s->rtspUrlW = rtspUrl;
    else
        s->rtspUrlW = L"";

    s->encoder = CreateNvEncoder(
        codec,
        g_device.Get(), g_context.Get(),
        s->w, s->h, s->fps, s->bitrate
    );
    if (!s->encoder || !s->encoder->Initialize()) {
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

    if (s->encoder) {
        std::vector<std::vector<uint8_t>> tail;
        s->encoder->Flush(tail);
        for (auto& p : tail) {
            send_annexb_packet_locked(*s, p.data(), p.size(), 0);
        }
        s->encoder.reset();
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
