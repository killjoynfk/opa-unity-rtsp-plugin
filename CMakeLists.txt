
cmake_minimum_required(VERSION 3.20)
project(NvencRtspPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(NvencRtspPlugin SHARED
    src/NvencRtspPlugin.cpp
    src/NvencRtspPlugin.h
)

target_include_directories(NvencRtspPlugin PRIVATE
    "C:/Program Files/Unity/Hub/Editor/2021.3.35f1/Editor/Data/PluginAPI"
    "nvidia/Interface"
    "ffmpeg/include"
    "nvidia/Samples/NvCodec/NvEncoder"
    "nvidia/Samples/Utils"
)

# Adjust libs / names to match your FFmpeg / NvCodec build
target_link_directories(NvencRtspPlugin PRIVATE
    "nvidia/Lib/x64"
    "ffmpeg/lib"
)

target_link_libraries(NvencRtspPlugin PRIVATE
    d3d11
    nvEncodeAPI
    avformat
    avutil
    ws2_32
    secur32
    bcrypt
    avcodec
    avfilter
)

# Unity expects plain name without 'lib' prefix
set_target_properties(NvencRtspPlugin PROPERTIES
    OUTPUT_NAME "NvencRtspPlugin"
)
