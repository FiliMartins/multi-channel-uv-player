cmake_minimum_required(VERSION 3.24)
project(MultiChannelUVPlayer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime so the .exe runs without installing the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9-docking
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR none)
FetchContent_Declare(ufbx
    GIT_REPOSITORY https://github.com/ufbx/ufbx.git
    GIT_TAG v0.23.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR none)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
    SOURCE_SUBDIR none)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
# imgui, ufbx and stb are plain sources: SOURCE_SUBDIR none fetches them without add_subdirectory.
FetchContent_MakeAvailable(glfw imgui ufbx stb json)

# FFmpeg reads video files and decodes H.264 and NotchLC; HAP frames are only
# demuxed by FFmpeg and decoded on the GPU. Windows uses a prebuilt LGPL shared
# build (FFMPEG_ROOT, fetched by CI), other platforms the system's pkg-config.
if(WIN32)
    set(FFMPEG_ROOT "" CACHE PATH "Folder of a shared FFmpeg build with include, lib and bin")
    if(NOT EXISTS "${FFMPEG_ROOT}/include/libavformat/avformat.h")
        message(FATAL_ERROR "Set FFMPEG_ROOT to a shared FFmpeg build (see .github/workflows/windows-build.yml).")
    endif()
    add_library(ffmpeg INTERFACE)
    target_include_directories(ffmpeg INTERFACE "${FFMPEG_ROOT}/include")
    target_link_libraries(ffmpeg INTERFACE "${FFMPEG_ROOT}/lib/avformat.lib" "${FFMPEG_ROOT}/lib/avcodec.lib"
                                           "${FFMPEG_ROOT}/lib/swscale.lib" "${FFMPEG_ROOT}/lib/avutil.lib")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()
find_package(Threads REQUIRED)

add_library(glad STATIC third_party/glad/src/gl.c)
target_include_directories(glad PUBLIC third_party/glad/include)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)

add_library(ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
target_include_directories(ufbx PUBLIC ${ufbx_SOURCE_DIR})

add_executable(MultiChannelUVPlayer
    src/main.cpp
    src/app.cpp
    src/app_project.cpp
    src/app_video.cpp
    src/mesh.cpp
    src/media.cpp
    src/video.cpp
    src/hap.cpp
    src/snappy.cpp
    src/renderer.cpp
    src/gl_util.cpp
    src/generators.cpp
    src/distortion.cpp
    src/file_dialog.cpp
    src/stb_impl.cpp)
target_include_directories(MultiChannelUVPlayer PRIVATE src ${stb_SOURCE_DIR})
target_link_libraries(MultiChannelUVPlayer PRIVATE glad imgui ufbx glfw nlohmann_json::nlohmann_json ffmpeg Threads::Threads)

if(WIN32)
    target_compile_definitions(MultiChannelUVPlayer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(MultiChannelUVPlayer PRIVATE comdlg32 ole32 shell32 opengl32)
    # Window app without a console; main() stays the entry point.
    set_target_properties(MultiChannelUVPlayer PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(MultiChannelUVPlayer PRIVATE /ENTRY:mainCRTStartup)
endif()
