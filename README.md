# Mosaic
<img src="assets/icon.png" width="100">

A desktop app for browsing and downloading from image boards, and for
managing the library you end up with. It's inspired by
[imgbrd-grabber](https://github.com/Bionus/imgbrd-grabber), but simpler,
and it keeps a local library you can search.

![Homepage](docs/images/Homepage.png)

---

## Features

- **Tabs**: one tab per source, or an "All sources" tab that searches
  several at once. Tag search has autocomplete, and thumbnails load as you
  scroll.
- **Viewer**: ← / → moves between posts, ↑ / ↓ follows parent/child posts,
  and videos can be stepped frame by frame.

  ![Viewer](docs/images/WindowViewer.png)

- **For You feed**: a vertical feed that learns from what you like, save and
  skip. The Explore slider controls how often it shows you something new.
- **Local library**: tags, favorites and metadata for what you've saved.
- **Duplicates**: finds near-identical images so you can pick which to keep.
- **Tag tools**: merge similar tags, fix conflicts, filter from a sidebar.
- **Web server**: *Tools → Web server* serves a mobile page on your network
  with the For You feed (double-tap to like) and library search. It trains
  the same recommender. Mosaic has to stay open.
- Images, video and animated posts all play back, with rendering on the GPU
  and prefetching in the background.

  ![Favorites](docs/images/Favorites.png)

---

## Building

See the **[build guide](src/README.md)** for prerequisites and commands.

| | |
|---|---|
| **Platform** | Windows (x64) |
| **Media** | Images and video |
| **Built with** | C++20 - Qt 6 - CMake |

Windows is the only supported target: the build files assume MSVC, and the
single-instance guard (`InstanceGuard`) is a no-op on other platforms.


## Keybinds

### Main window

| Key | Action |
|---|---|
| `Ctrl+Q` | Quit |
| `Ctrl+Shift+T` | Reopen closed tab |
| `F11` | Maximize / restore window |

### Viewer

| Key | Action |
|---|---|
| `←` / `→` | Previous / next post |
| `↑` / `↓` | Parent / child in the post chain |
| `Space` | Play / pause |
| `F11` | Toggle fullscreen |
| `F5` | Toggle slideshow |
| `,` / `.` | Scrub video back / forward one frame (`Shift` = 10 frames) |
| `<` / `>` | Scrub video back / forward one frame |
| `Ctrl+S` | Save to library (add `Shift` to also favorite) |
| `Ctrl+C` | Copy media |
| `Esc` | Close (closes loupe first if open) |
| `Ctrl+Q` | Close viewer |

### Feed ("For You")

| Key | Action |
|---|---|
| `Space` / `→` | Next post |
| `←` / `Backspace` | Previous post |
| `L` | Like and advance |
| `D` | Dislike and advance |
| `Ctrl+S` | Save to library and advance |
| `F12` | Toggle debug info |

The **Explore** slider next to the action buttons sets how much the feed
branches out. Left sticks to what you already like. Right fills more of each
batch with posts it's unsure about, like new tags or styles.

The debug sidebar (`F12`) shows how the learned model is doing: how much of
the ranking it controls, its accuracy compared to the heuristic, what it has
learned from your likes and dislikes, and the tangents coming up. It also has
**Dropped images**, which lists why posts were rejected, and **Liked posts**,
which opens everything you've liked or saved in the viewer. Liked posts load
in pages as you scroll, so a large history doesn't all load at once.

---
