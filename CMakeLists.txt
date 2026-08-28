cmake_minimum_required(VERSION 3.20)

project(SimpleMonitor LANGUAGES CXX RC)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(simple_monitor_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
        )
    endif()
endfunction()

add_executable(simple_monitor WIN32
    src/main.cpp
    app.rc
)

add_executable(simple_monitor_dev WIN32
    src/main-dev.cpp
    app.rc
)

set(SIMPLE_MONITOR_TARGETS
    simple_monitor
    simple_monitor_dev
)

set(SIMPLE_MONITOR_COMMON_SOURCES
    src/app_support.cpp
    src/logging.cpp
)

foreach(target IN LISTS SIMPLE_MONITOR_TARGETS)
    target_sources(${target} PRIVATE ${SIMPLE_MONITOR_COMMON_SOURCES})
    simple_monitor_enable_warnings(${target})

    target_link_libraries(${target} PRIVATE
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
endforeach()

target_link_libraries(simple_monitor_dev PRIVATE dwmapi)

if(BUILD_TESTING)
    add_executable(overlay_policy_tests
        tests/overlay_policy_tests.cpp
    )
    target_include_directories(overlay_policy_tests PRIVATE src)
    simple_monitor_enable_warnings(overlay_policy_tests)
    add_test(NAME overlay_policy_tests COMMAND overlay_policy_tests)

    add_executable(overlay_policy_trace_tests
        tests/overlay_policy_trace_tests.cpp
    )
    target_include_directories(overlay_policy_trace_tests PRIVATE src)
    simple_monitor_enable_warnings(overlay_policy_trace_tests)
    add_test(NAME overlay_policy_trace_tests COMMAND overlay_policy_trace_tests)

    add_executable(metric_refresh_policy_tests
        tests/metric_refresh_policy_tests.cpp
    )
    target_include_directories(metric_refresh_policy_tests PRIVATE src)
    simple_monitor_enable_warnings(metric_refresh_policy_tests)
    add_test(NAME metric_refresh_policy_tests COMMAND metric_refresh_policy_tests)
endif()

# Enable MinGW Unicode entry point handling so the app uses `wWinMain`
# instead of the ANSI `WinMain` startup path.
if(MINGW)
    foreach(target IN LISTS SIMPLE_MONITOR_TARGETS)
        target_compile_options(${target} PRIVATE
            -municode
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>
        )

        target_link_options(${target} PRIVATE
            -municode
            $<$<CONFIG:Release>:-Wl,--gc-sections>
        )
    endforeach()
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
    foreach(target IN LISTS SIMPLE_MONITOR_TARGETS)
        set_property(
            TARGET ${target}
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        )
    endforeach()

    message(STATUS "IPO/LTO enabled for simple_monitor Release build")
else()
    message(WARNING "IPO/LTO is not supported: ${IPO_ERROR}")
endif()
