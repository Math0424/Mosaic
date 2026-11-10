# Mosaic — Technical / Build Guide

This document is the developer reference: how to build Mosaic from source,
where everything lives, and the technical details behind the stack. For a
plain-language overview of what the app does, see the [root README](../README.md).

**Stack:** C++20 · Qt 6 (Widgets) · FFmpeg · OpenCV · DuckDB · CMake / Ninja / MSVC

Rendering is GPU-accelerated through Qt's RHI abstraction (`QOpenGLWidget`),
which picks the best backend per platform — D3D11 on Windows, Metal on
iOS/macOS, OpenGL ES / Vulkan on Android. There is no explicit Vulkan
dependency, so the same code base stays mobile-friendly.

---

## Requirements (Windows)

| Tool | Notes |
|---|---|
| Visual Studio 2022 | "Desktop development with C++" workload (MSVC + Windows SDK) |
| CMake ≥ 3.25 | `winget install Kitware.CMake` |
| Ninja | `winget install Ninja-build.Ninja` |
| Qt 6 (MSVC 2022 64-bit) | Qt Online Installer; the build requires **Qt ≥ 6.11** (`find_package(Qt6 6.11 REQUIRED)`) |
| Inno Setup *(optional)* | only needed for the `installer` target; `winget install JRSoftware.InnoSetup` |
| clang-format *(optional)* | only needed for the `format` target; `winget install LLVM.LLVM` |

Prebuilt third-party libraries (FFmpeg, OpenCV, DuckDB) are **not** checked
in — set them up as described below.

---

## Setup

### 1. Third-party libraries

Place each library under `third_party/libs/` with this layout:

```
third_party/libs/
├── ffmpeg/      # FFmpeg shared build
│   ├── bin/         *.dll
│   ├── include/     libavcodec/, libavformat/, libavutil/, libswscale/, ...
│   └── lib/         avcodec.lib, avformat.lib, avutil.lib, swscale.lib, ...
├── opencv/      # contents of the official opencv/build folder
│   ├── include/
│   ├── x64/vc16/bin/ + lib/
│   └── OpenCVConfig.cmake
└── duckdb/      # contents of libduckdb-windows-amd64.zip
    ├── duckdb.hpp
    ├── duckdb.lib
    └── duckdb.dll
```

Where to get them:

1. **FFmpeg** — download
   [`ffmpeg-master-latest-win64-gpl-shared.zip`](https://github.com/BtbN/FFmpeg-Builds/releases)
   and extract its `bin/`, `include/`, `lib/` folders into
   `third_party/libs/ffmpeg/` (see the README in that folder).
2. **OpenCV** — download the official Windows release from
   [opencv.org/releases](https://opencv.org/releases/), extract it, and make
   `third_party/libs/opencv` point at the extracted `build` folder. A
   directory junction works well:

   ```bat
   mklink /J third_party\libs\opencv C:\path\to\opencv\build
   ```

3. **DuckDB** — download `libduckdb-windows-amd64.zip` from
   [duckdb/duckdb releases](https://github.com/duckdb/duckdb/releases) and
   extract it into `third_party/libs/duckdb/`.

Verify everything is in place (any POSIX shell — Git Bash, MSYS2, WSL):

```sh
sh scripts/check_libs.sh
```

If CMake cannot find a library, configure fails with a message naming exactly
what is missing.

> **Keeping the libraries elsewhere?** The root is a CMake cache variable,
> `THIRDPARTY_ROOT` (defaults to `third_party/libs`). Override it at
> configure time, e.g. `cmake --preset dev -DTHIRDPARTY_ROOT=C:/Development/libs`.

### 2. Qt

Qt is located with the standard CMake mechanism (`find_package(Qt6)`), so no
paths are hardcoded in the repo. CMake finds it in this order
(see `cmake/qt_root.cmake`):

1. `CMAKE_PREFIX_PATH` — set it via environment, `-D` flag, or user presets
   (see below).
2. Default Qt installer locations are probed automatically
   (`C:/Qt/6.*`, `%USERPROFILE%/Qt/6.*`, `~/Qt/6.*`, `/opt/Qt/6.*`).

If your Qt lives somewhere unusual, copy `CMakeUserPresets.json.example` to
`CMakeUserPresets.json` (gitignored) and point it at your install:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "dev-local",
      "inherits": "dev",
      "cacheVariables": { "CMAKE_PREFIX_PATH": "D:/Qt/6.11.1/msvc2022_64" }
    }
  ]
}
```

Then use `dev-local` / `publish-local` instead of `dev` / `publish` below.

---

## Build

Use a terminal that has the MSVC environment available — e.g. the
*x64 Native Tools Command Prompt for VS 2022*, or run `vcvarsall.bat x64`
in your shell first.

```powershell
cmake --preset dev           # configure (Debug)
cmake --build --preset dev   # build

cmake --preset publish       # configure (MinSizeRel, no console window)
cmake --build --preset publish
```

The build automatically deploys everything needed to run next to the
executable (Qt via `windeployqt`, plus FFmpeg / OpenCV / DuckDB DLLs and
assets), so you can run it straight from the build tree:

```powershell
build\dev\bin\Mosaic.exe      # debug build
build\publish\bin\Mosaic.exe  # release build
```

### Presets

Defined in `CMakePresets.json`:

| Preset | Build type | Notes |
|---|---|---|
| `dev` | `Debug` | Emits `compile_commands.json` for tooling. |
| `publish` | `MinSizeRel` | `PUBLISH_NO_CONSOLE=ON` (GUI subsystem, no console window), interprocedural optimization on, `/O1 /Ob1 /DNDEBUG`, `/OPT:REF /OPT:ICF`. |

Compiler flags (MSVC, see `src/CMakeLists.txt`):

- `Mosaic` is compiled with `/W4 /permissive- /external:W0` — strict on our
  code, quiet on third-party headers.
- `Release`: `/O2 /GL` + `/LTCG` (link-time code generation),
  `/INCREMENTAL:NO`.
- `MinSizeRel`: `/O1 /Gy` + `/OPT:REF /OPT:ICF`.

### VS Code

`Ctrl+Shift+B` runs the default **Build Mosaic** task (same preset flow);
**Rebuild Mosaic**, **Build Publish**, and **Rebuild Publish** tasks are also
provided (see `.vscode/tasks.json`). The **Launch Mosaic (dev)** launch
configuration builds and debugs in one step (`.vscode/launch.json`).

### Packaging

All packaging targets are defined in `cmake/package.cmake` and build on top
of the `publish` preset.

**Portable zip**

```powershell
cmake --build --preset publish --target package-zip
```

Produces `build/publish/Mosaic-win-x64.zip` containing the executable, all
runtime DLLs, and assets. A `package` target (unzipped folder) is also
available.

**Windows installer (Inno Setup)**

```powershell
cmake --build --preset publish --target installer
```

Stages the same package folder, then compiles it into a proper Windows
installer with `ISCC` (Inno Setup 6 or 7). The result is
`build/installer/Mosaic-Setup-<version>.exe` — a standard setup
wizard with a Start-menu entry, an optional desktop shortcut, and
uninstall support. The installer script lives in `installer/Mosaic.iss`.

If Inno Setup isn't installed, CMake configures fine but the `installer`
target is skipped with a warning.

---

## Self-test

```powershell
build\dev\bin\Mosaic.exe --selftest
```

(Short flag: `-s`.) Runs a headless verification of every stack component —
Qt, the GPU / RHI context, OpenCV, FFmpeg, and a DuckDB schema round-trip
(see `app/Verification.cpp`). Exit code `0` means all checks passed.

The same checks run quietly on every normal start; failures are logged and
surfaced, but the app still launches.

---

## Code style

A `.clang-format` (Google-based, 4-space indent, 100-column limit) enforces
the code style. Format the whole source tree via the CMake target:

```powershell
cmake --build --preset dev --target format
```

VS Code formats on save with the same style file (see `.vscode/settings.json`).

---

## Project layout

```
Mosaic/
├── CMakeLists.txt          # top-level build: deps, flags, ffmpeg/duckdb targets
├── CMakePresets.json       # dev (Debug) and publish (MinSizeRel) presets
├── CMakeUserPresets.json.example  # local overrides (copy to CMakeUserPresets.json)
├── build_number.txt        # persistent build counter (bumped each build)
├── scripts/
│   └── check_libs.sh       # verifies third-party library setup
├── src/
│   ├── main.cpp            # entry point, --selftest flag, startup wiring
│   ├── app/                # application shell
│   │   ├── MainWindow.*    # tabbed workspace + status bar + menus
│   │   ├── Theme.*         # palette / styling
│   │   ├── InstanceGuard.* # single-instance enforcement
│   │   └── Verification.*  # stack self-test harness
│   ├── components/         # reusable UI widgets
│   │   ├── Icons.h         # asset icon loader (assets/*.svg)
│   │   ├── SourceBrowser.* # one source tab: tag search + thumbnail grid
│   │   ├── FavoritesPage.* # pinned favorites tab + sidebar
│   │   ├── ImportPage.*    # legacy library import tool
│   │   ├── ConflictsPage.* # duplicate-conflict resolution tool
│   │   ├── TagMergePage.*  # merge similar tags into one
│   │   ├── MultiSourceDialog.* # member picker for an "All sources" tab
│   │   ├── SettingsDialog.*# thumbnail grid geometry settings
│   │   ├── TagSidebar.*    # tag filter sidebar
│   │   ├── ViewerWindow.*  # standalone viewer: ←/→ posts, ↑/↓ parents, save
│   │   ├── ViewerWidget.*  # GPU-rendered frame display (QOpenGLWidget)
│   │   ├── MediaPlaybackController.* # video transport controls
│   │   ├── MediaCache.*    # shared byte-cache + prefetch pipeline
│   │   ├── FlowLayout.*    # wrapping thumbnail-grid layout
│   │   └── TagColors.h     # tag → color mapping
│   ├── media/
│   │   ├── VideoDecoder.*  # FFmpeg frame + audio decoder
│   │   └── ImageFingerprint.* # pHash / SIFT fingerprints for duplicate detection
│   ├── feed/               # "For You" recommender (self-contained)
│   │   ├── FeedPage.*      # TikTok-style vertical feed UI
│   │   ├── FeedRecommender.* # candidate arms, scoring, tangent slots, MMR
│   │   ├── FeedModel.*     # learned Bayesian model + Thompson sampling + arm bandit
│   │   ├── VisualFeatures.* # lightweight visual style descriptor (no ONNX)
│   │   ├── FeedWarmStart.* # seeds the model from history on first run
│   │   ├── FeedEngine.*    # shared feed DB + warm start + reaction rules (desktop & web)
│   │   └── FeedDatabase.*  # learning state (feed.duckdb)
│   ├── sources/            # pluggable image sources (remote + local)
│   │   ├── Source.h        # shared query/search/fetch interface
│   │   ├── SourceManager.* # registry of available sources
│   │   ├── MultiSource.*   # merged "All sources" source
│   │   ├── web/
│   │   │   ├── HttpSource.*# shared HTTP/JSON plumbing
│   │   │   ├── E621Source.*# e621.net JSON API source
│   │   │   ├── R34Source.* # rule34.us source
│   │   │   ├── Rule34XxxSource.* # rule34.xxx source
│   │   │   └── GelbooruSource.* # gelbooru / safebooru (dapi)
│   │   └── local/
│   │       ├── LibrarySource.* # local library source + save
│   │       ├── LibraryCatalog.*# catalog read/write over DuckDB
│   │       ├── LibraryTagService.* # tag CRUD + autocomplete
│   │       ├── LibraryImporter.* # import + remote tag lookup by md5
│   │       ├── DuplicateDetector.* # perceptual duplicate detection
│   │       ├── LibrarySql.*  # SQL helpers
│   │       └── LibraryUtils.*# md5 / file helpers
│   ├── web/                # local web app for phones / browsers
│   │   ├── HttpServer.*    # minimal HTTP/1.1 server (QTcpServer, keep-alive, Range)
│   │   ├── WebServer.*     # routes: library search/tags/thumbs, feed batches/events/media
│   │   ├── WebServerDialog.* # Tools > Web server: on/off, port, links
│   │   └── www/index.html  # the single-page app (embedded via web.qrc)
│   └── core/               # services (no UI)
│       ├── Paths.*         # AppData / library / temp directory resolution
│       ├── Database.*      # DuckDB connection + schema bootstrap
│       ├── Schema.*        # catalog DDL (images, tags, junction, indexes)
│       ├── AppState.*      # JSON UI-state persistence
│       └── FileLogger.*    # Qt log → %APPDATA%/Mosaic/Mosaic/Mosaic.log
├── cmake/
│   ├── qt_root.cmake       # locate Qt (CMAKE_PREFIX_PATH + installer probing)
│   ├── runtime.cmake       # windeployqt + runtime DLL deployment
│   ├── copy_runtime_dlls.cmake  # post-build FFmpeg/OpenCV/DuckDB copy
│   ├── build_number.cmake  # bump counter → generated/BuildNumber.h
│   ├── format.cmake        # clang-format "format" target
│   └── package.cmake       # packaging (package / package-zip / installer)
├── installer/
│   └── Mosaic.iss          # Inno Setup script for the Windows installer
├── assets/                 # SVG icons used by the UI
└── third_party/libs/       # prebuilt, not checked in (see README there)
```

---

## Runtime data layout

```
%APPDATA%/Mosaic/Mosaic/        # AppData (QStandardPaths::AppDataLocation)
├── state.json                # human-readable UI state (tabs, window, ...)
├── Mosaic.log                 # Qt log, pruned to the last 2 days
├── feed.duckdb               # "For You" learning state (separate from catalog)
└── library/                  # flat file store: <md5>.<ext>, no hierarchy
    └── Mosaic.duckdb          # catalog: images, tags, links, fingerprints

%TEMP%/Mosaic/                 # scratch space for in-flight downloads
```

Notes:

- **Library files are named by their MD5 hash** and stored flat (no
  subfolders). The catalog database (`Mosaic.duckdb`) lives *inside* the
  library folder so the whole library can be moved as one unit.
- **Single-writer catalog.** DuckDB allows one writer; `InstanceGuard`
  refuses to start a second Mosaic instance while the library is in use.
- **Feed state is isolated** in `feed.duckdb`, so the recommender can be
  removed without touching the library catalog.
- **Build number.** `build_number.txt` is a persistent counter bumped on
  every build; `cmake/build_number.cmake` emits `generated/BuildNumber.h`
  (`Mosaic_BUILD_NUMBER`) shown in the title bar.

---

## Technical notes

### Media pipeline

- **Images** are decoded with OpenCV (`imgcodecs`); **videos** with FFmpeg
  (`libavcodec` / `libavformat` / `libswscale` / `libswresample`) via
  `media/VideoDecoder`.
- **`MediaCache`** is a shared byte-cache + prefetch pipeline: it caches raw
  bytes per `<md5>:<kind>`, prefetches neighbouring posts off the GUI thread
  (global thread pool), and shows a cached preview while the full file loads.
  It is used by both the viewer and the feed.
- **`ViewerWidget`** renders the current frame on a `QOpenGLWidget` (Qt RHI).

### Duplicate detection

`media/ImageFingerprint` computes perceptual fingerprints (pHash and SIFT)
stored in the catalog; `sources/local/DuplicateDetector` uses them to surface
near-duplicate images for resolution in the Conflicts page.

### "For You" recommender

The feed starts on a fixed heuristic formula and hands over to a learned
model as the model proves itself:

- **`feed/FeedModel`**: online Bayesian probit regression (AdPredictor-style)
  with a Gaussian belief (mean + variance) per feature. The features are the
  post's tags, a 32-dimension visual style descriptor, and the heuristic's
  own signals (taste similarity, session, boredom, ...). Every reaction (like,
  save, long view, skip, dislike) is one weighted update.
- **Exploration through uncertainty**: ranking draws one weight vector from
  the posterior per batch (Thompson sampling). Unseen tags start wide, so the
  feed tries them. Variance slowly grows back toward the prior, so settled
  beliefs keep getting re-checked instead of locking the feed onto one path.
- **Evidence-gated handover**: before learning from each live reaction, both
  the heuristic (Platt-calibrated) and the model predict it. The running
  log-loss decides the blend, so the model only takes over while it actually
  predicts this user better, and backs off if it starts losing.
- **Tangents**: a share of each batch, set by the **Explore** slider on the
  feed page, goes to plausible but uncertain posts (optimism = mean + k ·
  uncertainty). A dedicated query arm probes "tangent" tags: tags seen on
  liked posts that no taste pursues yet. A liked tangent is promoted into
  its own taste.
- **Candidate arms** (taste, broad, combo, similar-to-liked, recent, popular,
  exploration, tangent, sway) get their network budget from a discounted
  Beta-Bernoulli bandit over how often each arm's posts are engaged with.
- **`feed/VisualFeatures`** computes the style descriptor (palette, tone,
  contrast, colorfulness, line density, detail, composition, aspect) from a
  thumbnail in plain C++. No pretrained network or ONNX runtime is shipped.
- **`feed/FeedWarmStart`** runs once in the background: the heuristic's tag
  preferences become model priors, library favorites replay as positives,
  and past reactions replay from `feed_events` (tags resolved by md5,
  rate-limited). It resumes across launches.

The heuristic stack (tag tastes, session vector, hard-negative filter) stays
in place: it is the cold-start prior and supplies inputs to the model. All
state lives in `feed.duckdb`.

### Web server

*Tools → Web server* (or `Mosaic.exe --serve 8787` for one session) serves
a mobile single-page app from inside the running app. It has to live
in-process: the catalog and `feed.duckdb` each allow only one open.

- **`web/HttpServer`** is a small HTTP/1.1 server on `QTcpServer` (Qt
  HttpServer is not required). Handlers run on the GUI thread and hand slow
  work to a pool; `HttpResponder` answers from any thread. Files stream in
  chunks with single-range support, so `<video>` can seek.
- **Feed**: the web feed uses its own `FeedRecommender` over the shared
  `FeedEngine` (DB, visual features, warm start), so desktop and web train
  one profile with the same dwell / skip / like rules. Remote media is
  proxied and cached in `%TEMP%/Mosaic/webmedia` (capped at 2 GB).
- **Library**: search uses the same syntax as the desktop (`-tag`, `~or`,
  `order:`); thumbnails are cached in `%TEMP%/Mosaic/webthumbs`.
- **Access**: this machine is always allowed. Other devices need the access
  key once (`?t=…` in the link from the dialog), then keep it in a cookie.
  The key is stored in `state.json` (`webServer`); "New access key" revokes
  old links. Plain HTTP on the LAN; there is no TLS.

### Sources

`sources/Source.h` defines the shared query / search / fetch interface.
Remote sources (e621, Rule34, and the gelbooru family via dapi) implement it
over HTTP/JSON; the local library implements it over DuckDB. `MultiSource`
merges several members into a single "All sources" tab, and `SourceManager`
is the registry the UI queries.
