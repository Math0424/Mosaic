# Third-party libraries

Drop libraries into `libs/` with this exact layout (see each subfolder's README):

```
libs/
├── ffmpeg/      # FFmpeg shared build (BtbN win64 GPL shared)
│   ├── bin/     #   *.dll
│   ├── include/ #   libavcodec/, libavformat/, libavutil/, libswscale/
│   └── lib/     #   avcodec.lib, avformat.lib, avutil.lib, swscale.lib
├── opencv/      # contents of the official opencv/build folder
│   ├── include/
│   ├── x64/vc16/bin/ + lib/
│   └── OpenCVConfig.cmake
└── duckdb/      # libduckdb-windows-amd64.zip contents
    ├── duckdb.hpp
    ├── duckdb.cpp (optional)
    ├── duckdb.lib
    └── duckdb.dll
```

CMake picks these up automatically via `THIRDPARTY_ROOT` (defaults to this
folder). If you keep them elsewhere, configure with:

```
cmake --preset dev -DTHIRDPARTY_ROOT=C:/Development/libs
```

Qt is **not** vendored here — it is found through the standard
`CMAKE_PREFIX_PATH` mechanism (see the root README). Run
`sh scripts/check_libs.sh` from the repo root to verify everything is in
place.
