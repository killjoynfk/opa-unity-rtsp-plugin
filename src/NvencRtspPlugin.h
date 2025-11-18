#pragma once
#include <stdint.h>

#ifdef _WIN32
  #define NVRTSP_EXPORT extern "C" __declspec(dllexport)
#else
  #define NVRTSP_EXPORT extern "C"
#endif

// callback в Unity для логирования
typedef void (*NvrtspLogCallback)(const char* msg);

// handle одного RTSP-энкодера/стрима
typedef void* NvrtspHandle;

// Установить callback логирования
NVRTSP_EXPORT void NVRTSP_SetLogCallback(NvrtspLogCallback cb);

// Создать инстанс стримера.
// texPtr      - ID3D11Texture2D* (RenderTexture.GetNativeTexturePtr())
// width/height, fps, bitrateKbps - параметры кодирования
// rtspUrl     - L"rtsp://127.0.0.1:8554/camXX"
NVRTSP_EXPORT NvrtspHandle NVRTSP_Create(
    void* texPtr,
    int width, int height, int fps,
    int bitrateKbps,
    const wchar_t* rtspUrl);

// Запустить фоновой поток стриминга.
NVRTSP_EXPORT bool NVRTSP_Start(NvrtspHandle handle);

// Остановить стриминг (останавливает фоновой поток, но handle ещё жив).
NVRTSP_EXPORT void NVRTSP_Stop(NvrtspHandle handle);

// Уничтожить handle, освободить все ресурсы.
NVRTSP_EXPORT void NVRTSP_Destroy(NvrtspHandle handle);
