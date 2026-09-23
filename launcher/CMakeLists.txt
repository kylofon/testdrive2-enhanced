# The game's launcher, "TD2 Enhanced": the game folder, how the game starts (the intro and menus, or a race
# at once on a chosen stage and car) and the enhanced view's options, then testdrive2-enhanced.
#
#   cmake -S launcher -B launcher/build -G Ninja && cmake --build launcher/build
#
# or, from the main project, -DTD2_LAUNCHER=ON builds it next to testdrive2-enhanced.exe.
# Needs wxWidgets 3.2 (MSYS2: mingw-w64-x86_64-wxwidgets3.2-msw; Debian and Ubuntu: libwxgtk3.2-dev).
cmake_minimum_required(VERSION 3.24)
project(td2launcher LANGUAGES CXX)
if(WIN32)
    enable_language(RC)
endif()

# MSYS2 and wxWidgets' own CMake builds install a package config; distributions
# such as Debian only ship wx-config, which CMake's FindwxWidgets uses.
find_package(wxWidgets 3.2 CONFIG QUIET COMPONENTS core base)
if(wxWidgets_FOUND AND TARGET wx::wxcore)
    set(TDL_WX_LIBRARIES wx::wxcore wx::wxbase)
else()
    find_package(wxWidgets 3.2 REQUIRED COMPONENTS core base)
    include(${wxWidgets_USE_FILE})
    set(TDL_WX_LIBRARIES ${wxWidgets_LIBRARIES})
endif()

add_executable(td2launcher WIN32
    app.cpp
    game.cpp
    icon.cpp
    launcher.cpp
    settings.cpp
)
set_target_properties(td2launcher PROPERTIES OUTPUT_NAME "TD2 Enhanced" CXX_EXTENSIONS OFF)
target_compile_features(td2launcher PRIVATE cxx_std_17)
target_link_libraries(td2launcher PRIVATE ${TDL_WX_LIBRARIES})
# Built from the main project: beside testdrive2-enhanced.exe, where it looks for it.
if(NOT PROJECT_IS_TOP_LEVEL)
    set_target_properties(td2launcher PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

if(WIN32)
    target_sources(td2launcher PRIVATE app.rc)
    target_link_libraries(td2launcher PRIVATE comctl32)
    if(MINGW)
        # The C++ runtime inside the exe, so the build needs fewer DLLs beside it.
        target_link_options(td2launcher PRIVATE -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
    endif()
    # The wxWidgets DLLs and everything they load, found where the compiler's own DLLs are.
    get_filename_component(TDL_DLL_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET td2launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:td2launcher> "-DSEARCH=${TDL_DLL_DIR}"
                -DCMAKE_OBJDUMP=${CMAKE_OBJDUMP} -P ${CMAKE_CURRENT_SOURCE_DIR}/copy_dlls.cmake)
endif()

if(MSVC)
    target_compile_options(td2launcher PRIVATE /W4 /utf-8)
else()
    target_compile_options(td2launcher PRIVATE -Wall -Wextra)
endif()
