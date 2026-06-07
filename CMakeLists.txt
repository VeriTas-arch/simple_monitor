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

if(MINGW)
    target_compile_options(simple_monitor PRIVATE -municode)
    target_link_options(simple_monitor PRIVATE -municode)
endif()
