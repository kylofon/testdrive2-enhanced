cmake_minimum_required(VERSION 3.24)
project(testdrive2_enhanced VERSION 0.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL3 REQUIRED CONFIG)

file(GLOB_RECURSE TD_SOURCES CONFIGURE_DEPENDS src/*.c)
file(GLOB_RECURSE TD_ENHANCED_SOURCES CONFIGURE_DEPENDS src/enhanced/*.c)

add_executable(testdrive2-enhanced ${TD_SOURCES})
target_include_directories(testdrive2-enhanced PRIVATE src)
target_link_libraries(testdrive2-enhanced PRIVATE SDL3::SDL3)

if(MSVC)
    target_compile_options(testdrive2-enhanced PRIVATE /W3)
else()
    # The memory model reads multi-byte values through byte pointers (see src/mem.h).
    target_compile_options(testdrive2-enhanced PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
    # Game strings are copied out of mem[] into fixed buffers with snprintf; truncation is intended.
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_compile_options(testdrive2-enhanced PRIVATE -Wno-format-truncation)
    endif()
    # The enhanced renderer does per-pixel work every frame; keep it optimised in Debug builds.
    if(TD_ENHANCED_SOURCES)
        set_source_files_properties(${TD_ENHANCED_SOURCES} PROPERTIES COMPILE_OPTIONS -O2)
    endif()
endif()

if(WIN32)
    # SDL3.dll, then what it loads in turn (MSYS2's SDL3.dll needs libiconv-2.dll), found where the compiler's
    # own DLLs are. Without it Windows takes whatever libiconv-2.dll is on the PATH; a 32-bit one fails with
    # 0xc000007b.
    get_filename_component(TD_DLL_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    add_custom_command(TARGET testdrive2-enhanced POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:testdrive2-enhanced> $<TARGET_FILE_DIR:testdrive2-enhanced>
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:testdrive2-enhanced> "-DSEARCH=${TD_DLL_DIR}"
                -DCMAKE_OBJDUMP=${CMAKE_OBJDUMP} -P ${CMAKE_CURRENT_SOURCE_DIR}/launcher/copy_dlls.cmake
        COMMAND_EXPAND_LISTS)
endif()

# The map viewer launcher ("TD2 Map Viewer"), which starts testdrive2-enhanced's map viewer (--viewer) on a
# chosen stage; built beside testdrive2-enhanced.exe. It needs wxWidgets 3.2.
option(TD2_VIEWER_LAUNCHER "Also build the map viewer launcher, TD2 Map Viewer.exe (wxWidgets)" OFF)
if(TD2_VIEWER_LAUNCHER)
    add_subdirectory(viewer-launcher)
endif()

# The game's launcher ("TD2 Enhanced"): the game folder, a race started at once on a chosen stage and car, and the
# enhanced view's options; built beside testdrive2-enhanced.exe. It needs wxWidgets 3.2.
option(TD2_LAUNCHER "Also build the game's launcher, TD2 Enhanced.exe (wxWidgets)" OFF)
if(TD2_LAUNCHER)
    add_subdirectory(launcher)
endif()
