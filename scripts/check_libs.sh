#!/bin/sh
# Verifies that all prebuilt third-party libraries are in place.
#
# Works in any POSIX shell: Git Bash / MSYS2, WSL, Linux, macOS.
#   sh scripts/check_libs.sh
#
# See third_party/libs/README.md for download and layout instructions.

root="$(cd "$(dirname "$0")/.." && pwd)/third_party/libs"

missing=0

# check <label> <glob relative to third_party/libs>
check() {
    # shellcheck disable=SC2086  # the glob must stay unquoted
    if ls "$root"/$2 >/dev/null 2>&1; then
        printf '  [ OK ]   %s\n' "$1"
    else
        printf '  [MISS]   %s  ->  third_party/libs/%s\n' "$1" "$2"
        missing=$((missing + 1))
    fi
}

check "FFmpeg headers"      "ffmpeg/include/libavcodec/avcodec.h"
check "FFmpeg import libs"  "ffmpeg/lib/avcodec.lib"
check "FFmpeg DLLs"         "ffmpeg/bin/*.dll"
check "OpenCV headers"      "opencv/include/opencv2/core.hpp"
check "OpenCV CMake config" "opencv/OpenCVConfig.cmake"
check "OpenCV DLLs"         "opencv/x64/*/bin/*.dll"
check "DuckDB header"       "duckdb/duckdb.hpp"
check "DuckDB import lib"   "duckdb/duckdb.lib"
check "DuckDB DLL"          "duckdb/duckdb.dll"

echo
if [ "$missing" -gt 0 ]; then
    echo "$missing check(s) failed. See third_party/libs/README.md for setup instructions."
    exit 1
fi

echo "All third-party libraries are in place."
