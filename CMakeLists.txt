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
    d2d1
    dwrite
    windowscodecs
    ole32
    oleaut32
    oleacc
)

# Enable MinGW Unicode entry point handling so the app uses `wWinMain`
# instead of the ANSI `WinMain` startup path.
if(MINGW)
    target_compile_options(simple_monitor PRIVATE
        -municode
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
    )

    target_link_options(simple_monitor PRIVATE
        -municode
        $<$<CONFIG:Release>:-Wl,--gc-sections>
    )
endif()

# Probe whether the active compiler and linker support IPO/LTO before
# enabling it for the Release configuration.
include(CheckIPOSupported)

check_ipo_supported(
    RESULT IPO_SUPPORTED
    OUTPUT IPO_ERROR
    LANGUAGES CXX
)

if(IPO_SUPPORTED)
    # Keep IPO/LTO scoped to Release builds to avoid slower debug iteration.
    set_property(
        TARGET simple_monitor
        PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    )

    message(STATUS "IPO/LTO enabled for simple_monitor Release build")
else()
    message(WARNING "IPO/LTO is not supported: ${IPO_ERROR}")
endif()
