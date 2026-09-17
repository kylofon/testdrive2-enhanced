cmake_minimum_required(VERSION 3.24)
project(testdrive2_enhanced C)

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
    add_custom_command(TARGET testdrive2-enhanced POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:testdrive2-enhanced> $<TARGET_FILE_DIR:testdrive2-enhanced>
        COMMAND_EXPAND_LISTS)
endif()
