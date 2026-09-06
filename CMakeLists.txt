cmake_minimum_required(VERSION 3.25)

project(Mosaic
    VERSION 0.1.0
    DESCRIPTION "Bulk image viewing and downloading platform"
    LANGUAGES CXX C
)

include("${CMAKE_SOURCE_DIR}/cmake/qt_root.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/runtime.cmake")

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")


if(MSVC)
    add_compile_options(
        $<$<CONFIG:MinSizeRel>:/O1>
        $<$<CONFIG:MinSizeRel>:/Gy>
    )

    add_link_options(
        $<$<CONFIG:MinSizeRel>:/OPT:REF>
        $<$<CONFIG:MinSizeRel>:/OPT:ICF>
    )
endif()

# ---------------------------------------------------------------------------
# Third-party roots
# ---------------------------------------------------------------------------

if(NOT DEFINED THIRDPARTY_ROOT)
    set(THIRDPARTY_ROOT "${CMAKE_SOURCE_DIR}/third_party/libs")
endif()

set(FFMPEG_ROOT
    "${THIRDPARTY_ROOT}/ffmpeg"
    CACHE PATH "Root of FFmpeg shared build"
)

set(OpenCV_DIR
    "${THIRDPARTY_ROOT}/opencv"
    CACHE PATH "Directory containing OpenCVConfig.cmake"
)

set(DUCKDB_ROOT
    "${THIRDPARTY_ROOT}/duckdb"
    CACHE PATH "Root of DuckDB release contents"
)

set(ONNXRUNTIME_ROOT
    "${THIRDPARTY_ROOT}/onnxruntime"
    CACHE PATH "Root of ONNX Runtime release contents"
)

# ---------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------

# Qt is located via cmake/qt_root.cmake (standard CMAKE_PREFIX_PATH
# discovery with fallback probing of default installer locations).

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    OpenGLWidgets
    Network
    Concurrent
    Multimedia
)

# ---------------------------------------------------------------------------
# OpenCV
# ---------------------------------------------------------------------------

if(MSVC)
    set(OpenCV_ARCH x64 CACHE STRING "OpenCV binary architecture" FORCE)
    set(OpenCV_RUNTIME vc16 CACHE STRING "OpenCV MSVC runtime variant" FORCE)
endif()

find_package(OpenCV REQUIRED COMPONENTS
    core
    imgproc
    imgcodecs
    features2d
)

# ---------------------------------------------------------------------------
# FFmpeg
# ---------------------------------------------------------------------------

find_path(
    FFMPEG_INCLUDE_DIR
    libavcodec/avcodec.h
    PATHS
        "${FFMPEG_ROOT}/include"
    NO_DEFAULT_PATH
)

find_library(
    AVCODEC_LIB
    avcodec
    PATHS
        "${FFMPEG_ROOT}/lib"
        "${FFMPEG_ROOT}/lib/x64"
    NO_DEFAULT_PATH
)

find_library(
    AVFORMAT_LIB
    avformat
    PATHS
        "${FFMPEG_ROOT}/lib"
        "${FFMPEG_ROOT}/lib/x64"
    NO_DEFAULT_PATH
)

find_library(
    AVUTIL_LIB
    avutil
    PATHS
        "${FFMPEG_ROOT}/lib"
        "${FFMPEG_ROOT}/lib/x64"
    NO_DEFAULT_PATH
)

find_library(
    SWSCALE_LIB
    swscale
    PATHS
        "${FFMPEG_ROOT}/lib"
        "${FFMPEG_ROOT}/lib/x64"
    NO_DEFAULT_PATH
)

find_library(
    SWRESAMPLE_LIB
    swresample
    PATHS
        "${FFMPEG_ROOT}/lib"
        "${FFMPEG_ROOT}/lib/x64"
    NO_DEFAULT_PATH
)

if(
    NOT FFMPEG_INCLUDE_DIR
    OR NOT AVCODEC_LIB
    OR NOT AVFORMAT_LIB
    OR NOT AVUTIL_LIB
    OR NOT SWSCALE_LIB
    OR NOT SWRESAMPLE_LIB
)
    message(FATAL_ERROR
        "FFmpeg not found under ${FFMPEG_ROOT}. "
        "See third_party/libs/ffmpeg/README.md."
    )
endif()

add_library(ffmpeg INTERFACE)

target_include_directories(ffmpeg
    SYSTEM INTERFACE
        "${FFMPEG_INCLUDE_DIR}"
)

target_link_libraries(ffmpeg
    INTERFACE
        "${AVCODEC_LIB}"
        "${AVFORMAT_LIB}"
        "${AVUTIL_LIB}"
        "${SWSCALE_LIB}"
        "${SWRESAMPLE_LIB}"
)

# ---------------------------------------------------------------------------
# DuckDB
# ---------------------------------------------------------------------------

set(DUCKDB_INCLUDE_DIR
    "${DUCKDB_ROOT}"
)

set(DUCKDB_LIB
    "${DUCKDB_ROOT}/duckdb.lib"
)

if(NOT EXISTS "${DUCKDB_INCLUDE_DIR}/duckdb.hpp")
    message(FATAL_ERROR
        "DuckDB header not found: ${DUCKDB_INCLUDE_DIR}/duckdb.hpp"
    )
endif()

if(NOT EXISTS "${DUCKDB_LIB}")
    message(FATAL_ERROR
        "DuckDB library not found: ${DUCKDB_LIB}"
    )
endif()

add_library(duckdb INTERFACE)

target_include_directories(duckdb
    SYSTEM INTERFACE
        "${DUCKDB_INCLUDE_DIR}"
)

target_link_libraries(duckdb
    INTERFACE
        "${DUCKDB_LIB}"
)

message(STATUS "DuckDB include: ${DUCKDB_INCLUDE_DIR}")
message(STATUS "DuckDB library: ${DUCKDB_LIB}")

# ---------------------------------------------------------------------------
# ONNX Runtime (visual embeddings for the feed)
# ---------------------------------------------------------------------------

set(ONNXRUNTIME_INCLUDE_DIR
    "${ONNXRUNTIME_ROOT}/include"
)

set(ONNXRUNTIME_LIB
    "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
)

if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
    message(FATAL_ERROR
        "ONNX Runtime header not found: ${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h"
    )
endif()

if(NOT EXISTS "${ONNXRUNTIME_LIB}")
    message(FATAL_ERROR
        "ONNX Runtime library not found: ${ONNXRUNTIME_LIB}"
    )
endif()

add_library(onnxruntime INTERFACE)

target_include_directories(onnxruntime
    SYSTEM INTERFACE
        "${ONNXRUNTIME_INCLUDE_DIR}"
)

target_link_libraries(onnxruntime
    INTERFACE
        "${ONNXRUNTIME_LIB}"
)

message(STATUS "ONNX Runtime include: ${ONNXRUNTIME_INCLUDE_DIR}")
message(STATUS "ONNX Runtime library: ${ONNXRUNTIME_LIB}")

# ---------------------------------------------------------------------------
# Mosaic
# ---------------------------------------------------------------------------

add_subdirectory(src)

include("${CMAKE_SOURCE_DIR}/cmake/format.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/package.cmake")
