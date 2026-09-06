# FFmpeg

Download: https://github.com/BtbN/FFmpeg-Builds/releases
File:     `ffmpeg-master-latest-win64-gpl-shared.zip`

Extract the zip; inside is a folder like `ffmpeg-master-latest-win64-gpl-shared/`
containing `bin/`, `include/`, `lib/`. Copy those three folders' contents so
this folder looks like:

```
third_party/libs/ffmpeg/
├── bin/      (avcodec-*.dll, avformat-*.dll, avutil-*.dll, swscale-*.dll, ...)
├── include/  (libavcodec/, libavformat/, libavutil/, libswscale/, ...)
└── lib/      (avcodec.lib, avformat.lib, avutil.lib, swscale.lib, ...)
```
