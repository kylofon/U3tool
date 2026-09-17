# The wxWidgets app, for Windows and Linux.
if(WIN32)
    enable_language(RC)
endif()

# MSYS2 and wxWidgets' own CMake builds install a package config; distributions
# such as Debian only ship wx-config, which CMake's FindwxWidgets uses.
find_package(wxWidgets 3.2 CONFIG QUIET COMPONENTS core base)
if(wxWidgets_FOUND AND TARGET wx::wxcore)
    set(U3_WX_LIBRARIES wx::wxcore wx::wxbase)
else()
    find_package(wxWidgets 3.2 REQUIRED COMPONENTS core base)
    include(${wxWidgets_USE_FILE})
    set(U3_WX_LIBRARIES ${wxWidgets_LIBRARIES})
endif()

add_executable(ultima3-assistant WIN32
    app.cpp
    carriedlist.cpp
    dialogs.cpp
    icon.cpp
    mainframe.cpp
    mapframe.cpp
    partycolumn.cpp
    referenceframe.cpp
    settings.cpp
)
set_target_properties(ultima3-assistant PROPERTIES OUTPUT_NAME "Ultima III Assistant" CXX_EXTENSIONS OFF)
target_compile_features(ultima3-assistant PRIVATE cxx_std_17)
target_link_libraries(ultima3-assistant PRIVATE u3core ${U3_WX_LIBRARIES})

if(WIN32)
    # The version number and the crash reporter still live with the Win32 app
    # until it's retired.
    target_include_directories(ultima3-assistant PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../u3stats-win32)
    target_sources(ultima3-assistant PRIVATE app.rc ../u3stats-win32/crash.cpp)
    target_link_libraries(ultima3-assistant PRIVATE comctl32 dbghelp)
    if(MINGW)
        target_link_options(ultima3-assistant PRIVATE "-Wl,-Map,Ultima III Assistant.map")
    endif()
else()
    target_include_directories(ultima3-assistant PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../u3stats-win32)
endif()

if(MSVC)
    target_compile_options(ultima3-assistant PRIVATE /W4 /utf-8)
else()
    target_compile_options(ultima3-assistant PRIVATE -Wall -Wextra)
endif()
