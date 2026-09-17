# Ultima III Assistant: the portable core and the wxWidgets app.
#
#   cmake -S . -B build && cmake --build build
#
# Needs wxWidgets 3.2 (MSYS2: mingw-w64-x86_64-wxwidgets3.2-msw; Debian and
# Ubuntu: libwxgtk3.2-dev). Pass -DU3_BUILD_APP=OFF to build only the core.
cmake_minimum_required(VERSION 3.16)
project(ultima3-assistant LANGUAGES CXX)

add_subdirectory(core)

option(U3_BUILD_APP "Build the wxWidgets app" ON)
if(U3_BUILD_APP)
    add_subdirectory(app)
endif()
