# copy_runtime_dlls.cmake
if(NOT DEFINED DEST_DIR)
    message(FATAL_ERROR "DEST_DIR not set. This script should be called via add_custom_command.")
endif()

file(MAKE_DIRECTORY "${DEST_DIR}")

set(ALL_DLLS "")

# FFmpeg - Only glob if the directory actually exists!
if(DEFINED FFMPEG_ROOT AND EXISTS "${FFMPEG_ROOT}")
    file(GLOB FFMPEG_DLLS
        "${FFMPEG_ROOT}/bin/*.dll"
        "${FFMPEG_ROOT}/bin/x64/*.dll"
        "${FFMPEG_ROOT}/*.dll"
    )
    list(APPEND ALL_DLLS ${FFMPEG_DLLS})
endif()

# OpenCV - Note the wildcard 'vc*' to catch MSVC 2019 (vc16) and 2022 (vc17)
if(DEFINED OpenCV_DIR AND EXISTS "${OpenCV_DIR}")
    file(GLOB OPENCV_DLLS
        "${OpenCV_DIR}/x64/vc*/bin/*.dll"
        "${OpenCV_DIR}/x64/*/bin/*.dll"
    )
    list(APPEND ALL_DLLS ${OPENCV_DLLS})
endif()

# DuckDB
if(DEFINED DUCKDB_ROOT AND EXISTS "${DUCKDB_ROOT}")
    file(GLOB DUCKDB_DLLS
        "${DUCKDB_ROOT}/*.dll"
    )
    list(APPEND ALL_DLLS ${DUCKDB_DLLS})
endif()

# Clean up the list: remove empty items, system files, and weird paths
set(CLEAN_DLLS "")
foreach(DLL IN LISTS ALL_DLLS)
    # Skip empty strings
    if(NOT DLL)
        continue()
    endif()
    
    # Skip paths that start with // (invalid UNC or malformed)
    if(DLL MATCHES "^//")
        message(STATUS "Skipping malformed path: ${DLL}")
        continue()
    endif()

    # Skip anything located in Windows\System32 (prevents copying OS dlls)
    if(DLL MATCHES "Windows[/\\]System32")
        continue()
    endif()

    # Only keep files that actually exist on disk
    if(EXISTS "${DLL}")
        list(APPEND CLEAN_DLLS "${DLL}")
    endif()
endforeach()

foreach(DLL IN LISTS CLEAN_DLLS)
    get_filename_component(NAME "${DLL}" NAME)
    
    # Optional: If you strictly want Release builds and want to skip debug DLLs:
    # if(NAME MATCHES "d\\.dll$")
    #     continue()
    # endif()

    file(COPY "${DLL}" DESTINATION "${DEST_DIR}")
    message(STATUS "Runtime copied: ${NAME}")
endforeach()