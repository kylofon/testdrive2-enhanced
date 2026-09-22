# Copies every DLL the launcher needs from outside Windows (the wxWidgets DLLs and what they
# load in turn: libstdc++, libpng, libtiff, ...) next to it, so it starts without MSYS2 on the PATH.
#   cmake -DEXE=<launcher> -DSEARCH=<dir with the DLLs> [-DCMAKE_OBJDUMP=<objdump>] -P copy_dlls.cmake
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
get_filename_component(dest "${EXE}" DIRECTORY)
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${EXE}"
    DIRECTORIES ${SEARCH}
    RESOLVED_DEPENDENCIES_VAR found
    UNRESOLVED_DEPENDENCIES_VAR missing
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "[/\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\]")
foreach(dll IN LISTS found)
    file(COPY "${dll}" DESTINATION "${dest}")
endforeach()
if(missing)
    message(WARNING "Not found, not copied: ${missing}")
endif()
