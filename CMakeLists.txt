cmake_minimum_required(VERSION 3.25)
project(WaveMind LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# vcpkg provides these
find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Header-only packages located via find_path
find_path(MINIAUDIO_INCLUDE_DIR NAMES miniaudio.h REQUIRED)
find_path(STB_INCLUDE_DIR NAMES stb_image.h REQUIRED)

add_executable(WaveMind WIN32
    src/main.cpp
    src/key_store.cpp
    src/audio_capture.cpp
    src/vad_chunker.cpp
    src/wav.cpp
    src/transcriber.cpp
    src/media_decoder.cpp
    src/image_loader.cpp
    resource.rc
)

target_compile_features(WaveMind PRIVATE cxx_std_23)

target_compile_definitions(WaveMind PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_include_directories(WaveMind PRIVATE
    src
    ${MINIAUDIO_INCLUDE_DIR}
    ${STB_INCLUDE_DIR}
)

target_link_libraries(WaveMind PRIVATE
    imgui::imgui
    glfw
    CURL::libcurl
    nlohmann_json::nlohmann_json
    opengl32
    Crypt32
    Shell32
    comdlg32
    Dwmapi
    ole32
    oleaut32
    ksuser
    # Media Foundation (file/video audio decoding)
    mfplat
    mfreadwrite
    mfuuid
    wmcodecdspuuid
    Avrt
)

if(MSVC)
    target_compile_options(WaveMind PRIVATE /W3 /MP /utf-8)
    target_compile_options(WaveMind PRIVATE /Zc:__cplusplus)
endif()
