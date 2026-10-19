cmake_minimum_required(VERSION 3.16)

# dnr — VapourSynth 4 插件（G3，mpv 集成）
#
# 只依赖两样东西：
#   1) VapourSynth API 4 头文件（仓库自带 third_party/vapoursynth/include，
#      也可以指向系统安装的 /usr/include/vapoursynth 或 VS 自编译安装目录）
#   2) libdnr（wolf-builder 提供的 C++/Vulkan 核心），或 -DDNR_USE_STUB=ON 用直通占位
#
# 典型用法：
#   cmake -S . -B build -DDNR_LIBRARY=/home/retro/dnr/lib/libdnr.so
#   cmake --build build -j
#   cmake --install build --prefix ~/.local
#
# 说明：VapourSynth 插件**不需要**链接 libvapoursynth/libvsscript——
# 宿主通过 VapourSynthPluginInit2 把 VSPLUGINAPI 传进来，因此只需要头文件。

project(dnr_vs_plugin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DNR_USE_STUB "不链接 libdnr，改用直通占位实现（只验证集成链路）" OFF)
set(DNR_LIBRARY "" CACHE FILEPATH "libdnr 的完整路径（.so/.a）；留空则自动查找")
set(DNR_VS_PLUGIN_INSTALL_DIR "lib/vapoursynth" CACHE STRING
    "插件安装子目录（相对 prefix）；VapourSynth 官方 autoload 目录是 <site-packages>/vapoursynth/plugins")

# ---------------------------------------------------------------- headers
find_path(VAPOURSYNTH_INCLUDE_DIR
    NAMES VapourSynth4.h
    HINTS
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vapoursynth/include
        /usr/local/include/vapoursynth
        /usr/include/vapoursynth
    DOC "VapourSynth API 4 头文件目录")

if(NOT VAPOURSYNTH_INCLUDE_DIR)
    message(FATAL_ERROR
        "找不到 VapourSynth4.h。\n"
        "  · 仓库内应有 third_party/vapoursynth/include/VapourSynth4.h（已随仓库提供）\n"
        "  · 或安装自编译的 VapourSynth 后指定 -DVAPOURSYNTH_INCLUDE_DIR=/path/to/include")
endif()
message(STATUS "VapourSynth headers: ${VAPOURSYNTH_INCLUDE_DIR}")

# ---------------------------------------------------------------- libdnr
set(DNR_LINK_TARGET "")
if(DNR_USE_STUB)
    message(STATUS "dnr: DNR_USE_STUB=ON，使用直通占位实现（不会真正降噪）")
else()
    if(DNR_LIBRARY)
        if(NOT EXISTS "${DNR_LIBRARY}")
            message(FATAL_ERROR "DNR_LIBRARY 指向的文件不存在: ${DNR_LIBRARY}")
        endif()
        message(STATUS "dnr: 链接 ${DNR_LIBRARY}")
        set(DNR_LINK_TARGET "${DNR_LIBRARY}")
    else()
        find_library(DNR_LIBRARY_AUTO NAMES dnr libdnr
            HINTS
                ${CMAKE_CURRENT_SOURCE_DIR}/../lib
                ${CMAKE_CURRENT_SOURCE_DIR}/../../build
                /usr/local/lib
                /opt/dnr/lib
            DOC "libdnr 库文件")
        if(DNR_LIBRARY_AUTO)
            message(STATUS "dnr: 自动找到 libdnr: ${DNR_LIBRARY_AUTO}")
            set(DNR_LINK_TARGET "${DNR_LIBRARY_AUTO}")
        else()
            message(WARNING
                "dnr: 没有找到 libdnr，退化为直通占位实现（DNR_USE_STUB）。\n"
                "     拿到 libdnr.so 后用 -DDNR_LIBRARY=/path/libdnr.so 重新 configure。")
            set(DNR_USE_STUB ON)
        endif()
    endif()
endif()

# ---------------------------------------------------------------- target
add_library(dnr_vs MODULE src/vs_plugin.cpp)

set_target_properties(dnr_vs PROPERTIES
    PREFIX ""
    OUTPUT_NAME "dnr"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

target_include_directories(dnr_vs PRIVATE
    "${VAPOURSYNTH_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(DNR_USE_STUB)
    target_sources(dnr_vs PRIVATE src/dnr_stub.cpp)
    target_compile_definitions(dnr_vs PRIVATE DNR_BUILD_STUB=1)
else()
    target_link_libraries(dnr_vs PRIVATE "${DNR_LINK_TARGET}")
    # 让插件能在同目录找到 libdnr.so（可选）：安装/构建后按需设置 RPATH。
    set_target_properties(dnr_vs PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH OFF)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dnr_vs PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(dnr_vs PRIVATE -fvisibility=hidden)
endif()

install(TARGETS dnr_vs LIBRARY DESTINATION "${DNR_VS_PLUGIN_INSTALL_DIR}")

# ---------------------------------------------------------------- summary
message(STATUS "-----------------------------------------------------------")
message(STATUS " dnr VS plugin: ${CMAKE_PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "   输出        : ${CMAKE_BINARY_DIR}/dnr.so")
message(STATUS "   VapourSynth : API 4 headers @ ${VAPOURSYNTH_INCLUDE_DIR}")
if(DNR_USE_STUB)
    message(STATUS "   核心        : STUB（直通，画面不变）")
else()
    message(STATUS "   核心        : ${DNR_LINK_TARGET}")
endif()
message(STATUS "   安装目录    : <prefix>/${DNR_VS_PLUGIN_INSTALL_DIR}")
message(STATUS "   示例脚本    : dnr.vpy（本仓库根目录）")
message(STATUS "-----------------------------------------------------------")
