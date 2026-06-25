# TL2 Loot Filter —— clone 即编(CMake 自动拉 ImGui + MinHook)
#
# 配置(必须 32 位,游戏是 x86):
#   cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
#   cmake --build build --config Release
# 产物:build/Release/d3d9.dll
cmake_minimum_required(VERSION 3.20)
project(tl2_loot_filter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
# MinHook(自带 CMake,产出 target: minhook)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master)
# Dear ImGui(纯源码,无 CMake)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5)
FetchContent_MakeAvailable(minhook imgui)

set(IMGUI_SRC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)

add_library(d3d9 SHARED
    hooks.cpp
    overlay.cpp
    loot_filter.cpp
    hook_b.cpp
    ${IMGUI_SRC}
    d3d9.def)              # 导出转发(Wrap_Direct3DCreate9 等)

target_include_directories(d3d9 PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

target_link_libraries(d3d9 PRIVATE minhook)   # d3d9 调用走 GetProcAddress,无需链接 d3d9.lib

# 输出名必须是 d3d9.dll(无 lib 前缀);UNICODE。
set_target_properties(d3d9 PROPERTIES OUTPUT_NAME "d3d9" PREFIX "")
target_compile_definitions(d3d9 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
