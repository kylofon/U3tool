# The platform-neutral part of Ultima III Assistant: reading and editing the
# game through DOSBox, and the game's reference and map data. No UI.
#
# Standalone:  cmake -S core -B build/core && cmake --build build/core
cmake_minimum_required(VERSION 3.16)
project(u3core LANGUAGES CXX)

add_library(u3core STATIC
    reader.cpp
    staging.cpp
    mapdata.cpp
    refdata.cpp
    platform_win32.cpp
    platform_posix.cpp
)
target_include_directories(u3core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(u3core PUBLIC cxx_std_17)
set_target_properties(u3core PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(u3core PRIVATE /W4 /utf-8)
else()
    target_compile_options(u3core PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    # cpp-httplib needs Windows 10 or later.
    target_compile_definitions(u3core PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX UNICODE _UNICODE)
    target_link_libraries(u3core PUBLIC ws2_32 iphlpapi ntdll)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(u3core PUBLIC Threads::Threads)
endif()
