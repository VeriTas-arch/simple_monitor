cmake_minimum_required(VERSION 3.20)

project(SimpleMonitor LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(simple_monitor WIN32
    src/main.cpp
    app.rc
)

target_link_libraries(simple_monitor PRIVATE
    user32
    gdi32
    shell32
    advapi32
    iphlpapi
    pdh
    shlwapi
    d2d1
    dwrite
    windowscodecs
    ole32
)

# MinGW Unicode 入口点支持：
# 使用 wWinMain，而不是 WinMain。
if(MINGW)
    target_compile_options(simple_monitor PRIVATE
        -municode
    )

    target_link_options(simple_monitor PRIVATE
        -municode
    )
endif()

# 检查当前编译器和链接器是否支持 IPO/LTO。
include(CheckIPOSupported)

check_ipo_supported(
    RESULT IPO_SUPPORTED
    OUTPUT IPO_ERROR
    LANGUAGES CXX
)

if(IPO_SUPPORTED)
    # 仅在 Release 配置中启用 IPO/LTO。
    set_property(
        TARGET simple_monitor
        PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    )

    message(STATUS "IPO/LTO enabled for simple_monitor Release build")
else()
    message(WARNING "IPO/LTO is not supported: ${IPO_ERROR}")
endif()
