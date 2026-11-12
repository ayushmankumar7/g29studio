cmake_minimum_required(VERSION 3.21)
project(g29studio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(g29studio WIN32
    src/main.cpp
    src/wheel.cpp
    src/profile.cpp
    src/testdrive.cpp
    src/ffbcal.cpp
    src/pad.cpp
    extern/ViGEmClient/src/ViGEmClient.cpp

    extern/imgui/imgui.cpp
    extern/imgui/imgui_draw.cpp
    extern/imgui/imgui_tables.cpp
    extern/imgui/imgui_widgets.cpp
    extern/imgui/backends/imgui_impl_win32.cpp
    extern/imgui/backends/imgui_impl_dx11.cpp
)

target_include_directories(g29studio PRIVATE
    src
    extern/imgui
    extern/imgui/backends
    extern/ViGEmClient/include
)

# DirectInput and Direct3D 11 both ship in the Windows SDK -- nothing to install.
target_link_libraries(g29studio PRIVATE
    dinput8 dxguid
    d3d11 dxgi d3dcompiler
    ole32 oleaut32 user32 gdi32 shell32 winmm setupapi
)

target_compile_definitions(g29studio PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(g29studio PRIVATE /utf-8 /W3 /EHsc /wd4828 /wd4644)
endif()
