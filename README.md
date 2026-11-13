# FrameStudio

A desktop app to cut and rearrange videos and export them at the quality or
size you want, with a **preview of the final file size** before exporting.

Handy for staying under the size limits of email, chat apps or upload
platforms: set the limit in MB and the app finds the best quality that fits,
or let it analyze the video and choose everything by itself.

FrameStudio was called *Video Compressor* up to version 2.4: an installed
Video Compressor updates itself to FrameStudio.

<p align="center">
  <!-- taken automatically for every release (see .github/workflows/build.yml): this is always the latest one -->
  <img src="https://github.com/Andrea332/FrameStudio/releases/latest/download/FrameStudio-screenshot.png"
       alt="FrameStudio with a video loaded: preview frame, automatic settings, settings and estimated output size" width="640">
</p>

## Download

**[Download the latest version](https://github.com/Andrea332/FrameStudio/releases/latest)**
for Windows (x64 or ARM64), macOS (Apple Silicon) or Linux (x86_64 or ARM64).
Previous versions and the changes in each one are on the
[releases page](https://github.com/Andrea332/FrameStudio/releases).
FFmpeg is included in every download: there is nothing else to install.

Every system also has a **portable** version (the files ending in
`-portable`): extract it and run it from wherever you like, even a USB stick,
without installing anything.

### Windows 10/11

There are two versions: `win64` for most PCs (Intel and AMD processors) and
`win-arm64` for Windows 11 PCs with an ARM processor, such as Snapdragon X
laptops. If you are not sure, open **Settings → System → About**: *System type*
says "x64-based processor" or "ARM-based processor".

- **Installer** (`FrameStudio-<version>-win64.exe` or `-win-arm64.exe`, the
  smaller download): run it and follow the steps. By default it installs for
  your user only, with no administrator rights; you can choose to install for
  all users instead. It adds FrameStudio to the Start menu (and, if you want,
  to the desktop), and you can uninstall it from Windows Settings like any
  other app. The app updates itself from then on (see *Updates* below).
- **Portable** (`FrameStudio-<version>-win64-portable.zip` or
  `-win-arm64-portable.zip`): extract it and run `FrameStudio.exe` inside the
  extracted folder. Keep the folder as it is, because the program needs the
  files next to it (Qt libraries and the `ffmpeg` folder). It keeps its
  settings in `FrameStudio.ini` in the same folder and writes nothing to the
  registry, so it leaves no trace on the PC (delete that file to keep the
  settings in the registry instead).

The app is not code-signed, so Windows SmartScreen may show "Windows protected
your PC" the first time: click **More info** → **Run anyway**.

### macOS 13 or later, Apple Silicon (M1 or later)

Open `FrameStudio-<version>-macos-arm64.dmg` and drag FrameStudio into
Applications. The portable version, `FrameStudio-<version>-macos-arm64-portable.zip`,
is the same app in a zip: extract it and run it from any folder.

The app is not signed by a registered Apple developer, so the first time macOS
blocks it. Open **System Settings → Privacy & Security**, scroll down and click
**Open Anyway** next to the message about FrameStudio, then confirm. This is
needed only once.

### Linux (x86_64 or ARM64)

Download `FrameStudio-<version>-linux-x86_64.AppImage` (most PCs) or
`-linux-aarch64.AppImage` (ARM64, for example a Raspberry Pi 4 or 5 with a
64-bit system), make it executable and run it:

```bash
chmod +x FrameStudio-*.AppImage
./FrameStudio-*.AppImage
```

The x86_64 version works on distributions from 2022 on (for example Ubuntu
22.04, Debian 12, Fedora 36 and later), the ARM64 one on distributions from 2024
on (Ubuntu 24.04, Debian 13, Fedora 40, Raspberry Pi OS based on Debian 13 and
later). If it doesn't start because FUSE is missing, run it with
`--appimage-extract-and-run`, or use the portable version.

The portable version, `FrameStudio-<version>-linux-x86_64-portable.tar.gz` (or
`-linux-aarch64-portable.tar.gz`), is the same app as a folder, which needs no
FUSE: extract it anywhere and run `FrameStudio` inside it.

```bash
tar -xzf FrameStudio-*-portable.tar.gz
./FrameStudio-*-portable/FrameStudio
```

It keeps its settings in `FrameStudio.ini` in that folder instead of in
`~/.config`.

## Features

- **Editor**: cut, copy, paste, delete and move parts of the video, with undo,
  then export it, or save it as it is without re-encoding (see
  [Editor](#editor) below).
- **Automatic settings**: the app analyzes the video and sets everything by
  itself, for the best quality within your size limit or for the smallest file
  with no visible loss (see [Automatic settings](#automatic-settings)).
- **Reliable output size estimate**: instead of a theoretical calculation, the
  app actually encodes a few short samples of the video with the chosen
  settings. This makes the estimate work well with any kind of content (screen
  recordings, camera footage, animations…). Videos up to 20 seconds are
  encoded whole, so the estimate is exact.
- **Fit quality to limit**: enter a maximum size in MB (in *Automatic
  settings*, with the goal *Best quality within the limit*) and the app
  searches for the highest quality that stays under it, keeping your other
  settings.
- **Constant quality or a bitrate of your choice**: by default the encoder
  keeps the chosen quality and spends the bits each scene needs, which gives the
  best quality for the size. You can instead set the video bitrate in kbps,
  where the encoder has these modes:
  - **Variable bitrate (VBR)**: the average you set, more for complex scenes
    and less for simple ones (up to twice the average). Every encoder has it.
  - **Constant bitrate (CBR)**: the same bitrate all the time, for streaming or
    for devices that need it. x264, x265, VP9 and the GPU encoders have it;
    SVT-AV1, VVenC and Xvid don't. GPU modes are offered only if the GPU accepts
    them.

  With a bitrate, the size is simply bitrate × duration, so it is shown right
  away, and *Fit bitrate to limit* computes the bitrate that fits.
- **Many output formats and codecs**:

  | Format | Video codecs | Audio codecs |
  |--------|--------------|--------------|
  | MP4    | H.264, H.265/HEVC, AV1, VP9, H.266/VVC, MPEG-4 (Xvid) | AAC, Opus, MP3 |
  | MKV    | H.264, H.265/HEVC, AV1, VP9, H.266/VVC, MPEG-4 (Xvid) | AAC, Opus, MP3, Vorbis |
  | WebM   | AV1, VP9 | Opus, Vorbis |
  | MOV    | H.264, H.265/HEVC | AAC, MP3 |
  | AVI    | MPEG-4 (Xvid) | MP3 |

  Only combinations that work together are offered. The default, MP4 with
  H.264 and AAC, plays almost everywhere.
- **Hardware acceleration**: codecs that a GPU in your computer can encode are
  marked with ⚡: NVIDIA NVENC (Windows, Linux), AMD AMF (Windows), Intel Quick
  Sync (Windows, Linux) for H.264, H.265 and AV1, and Apple VideoToolbox (macOS)
  for H.264 and H.265. Acceleration can be:
  - **Automatic**: the best GPU that can encode the chosen codec (NVIDIA, then
    Apple, Intel and AMD), otherwise the CPU;
  - **Manual**: you choose the GPU;
  - **Off**: always the CPU.

  The window always states which codec will be used, whether hardware
  acceleration is on and which device (GPU or CPU, by name) does the work.
  GPU encoding is much faster, but files are a bit larger at the same quality.
  The ARM64 versions for Windows and Linux encode on the CPU.
- **Any input video**: anything FFmpeg can read, including phone videos
  recorded in portrait, files with cover art and files with subtitles. The
  main video and the first audio track are kept; subtitles are not copied.
- **Simple settings**: quality, resolution, frame rate, audio codec and
  bitrate (or no audio) and encoding speed. Only sensible options are offered
  for the loaded video (no upscaling, no frame rate higher than the original),
  and vertical videos are handled correctly.
- **Smooth seeking**: every encoder places keyframes at the same, chosen
  interval (every 10 s by default for the smallest file, or 5, 2 or 1 s for
  faster and more precise seeking when editing or streaming). Keyframes are
  closed-GOP (IDR) points, and the seek index is written at the start of the
  file (MP4, MOV, MKV, WebM), so players can jump anywhere right away, even
  over the network. Shorter intervals make the file bigger, especially for
  videos with little motion such as screen recordings.
- **Preview of the source video**: as soon as the path points to a video, a
  frame of it (at 10% of its length) appears next to it.
- **Hard to miss when it's done**: at the end a green box shows the new file,
  its size and how much smaller it is than the original, with buttons to play
  it or open its folder (orange if it came out over the limit, red with the
  reason if FFmpeg failed). If the window is in the background, its taskbar
  button flashes (on macOS the Dock icon bounces). While the size is being
  estimated, a wheel turns next to it.
- Drag and drop a video onto the window, progress bar and cancel button.
- **FFmpeg included**: the packaged app needs nothing else installed.

## Automatic settings

At the top of the **Export** tab, choose a goal and click **Analyze and
apply**:

- **Best quality within the limit**: the app looks for the settings that stay
  under the limit you set (right below the goal, in MB) and look as good as
  possible. It tries
  the original size and frame rate, then lower ones (30 fps first, then a
  smaller picture), each at a good quality, and keeps the first that fits;
  then it raises the quality as far as the limit allows. A smaller picture that
  looks clean is better than the original size full of compression artifacts.
- **Smallest file with no visible loss**: the original size and frame rate at
  the quality where the result still looks like the original, whatever the
  limit (which is then hidden). If the video is already well compressed and
  exporting it would save little, it says so.

Either way the sound gets a bitrate that suits its channels (mono, stereo or
surround), never more than the original. The analysis also tells what it
found: size, frame rate and bitrate of the video, how much motion and detail it
has, and its sound. Format and codec stay as you chose them, unless you tick
**It may also change format and codec**: then it picks the most efficient codec
there is (AV1, otherwise H.265), which gives smaller files but doesn't play on
some older devices. Changing any setting by hand while it works stops it.

## Editor

<p align="center">
  <!-- taken automatically for every release, like the picture at the top -->
  <img src="https://github.com/Andrea332/FrameStudio/releases/latest/download/FrameStudio-editor.png"
       alt="The Editor tab: video preview, playback controls, the timeline with three pieces, and the editing buttons" width="640">
</p>

The **Editor** tab shows the video with its sound, and the timeline below it:
the edited video as a strip of pieces of the original, with thumbnails.

With the mouse:

- **Left button**: click or drag anywhere on the timeline (or grab the red
  playhead) to move the playhead; the video follows, frame by frame. Drag a
  selection to move it somewhere else, or its edges to change it. Shift+click
  extends the selection, double-click selects a piece.
- **Right button**: drag to select a part; a click opens a menu with cut,
  copy, paste, delete, split and the other actions.
- **Ctrl+wheel** zooms in on the timeline (also the − / + / Fit buttons). Zoomed
  in, the wheel moves back and forth along the video, and a thin bar appears
  under the timeline: it shows the whole video, with the part on screen as a box
  that you can drag (or click elsewhere on the bar to jump there). The
  thumbnails follow the zoom: each one is the frame actually under it.

Under the timeline the buttons are grouped by what they work on: **At the
playhead** (Split, Paste) and **Selection** (Start, End, Cut, Copy, Delete; the
title shows how long the selection is), then Undo and Redo.

Every action also has a key, which works anywhere in the Editor tab:

| Key | Does |
|-----|------|
| Space | Play or pause |
| Left / Right | One frame back or forward (Shift: one second) |
| Home / End | To the start or the end |
| I / O | The selection starts or ends at the playhead |
| S | Splits the piece at the playhead in two |
| Ctrl+X / Ctrl+C / Ctrl+V | Cuts, copies the selection, or pastes it at the playhead |
| Del | Removes the selection |
| Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) | Undo, redo |
| Ctrl+A / Esc | Selects everything, or nothing |

When the video is edited, **Export** encodes only what is left, in the order of
the timeline, cutting exactly on the frames you chose; the size estimate, the
automatic settings and *Fit to limit* work on the edited video. **Save without
re-encoding** saves the edited video right away in the original format and
quality. Without re-encoding, though, a piece can only start on a keyframe (the
marks under the timeline): a piece that starts elsewhere starts at the keyframe
before it, and the app tells you before saving.

## Updates

A few seconds after starting, at most once a day, the app asks GitHub which is
the latest release; nothing about you or your videos is sent. If there is a
newer version, a notice appears at the top of the window:

- **Update now** (Windows, installed with the installer): downloads the new
  installer, checks it against the SHA-256 published by GitHub, installs it
  and restarts the app.
- **Download** (portable versions, macOS, Linux): opens the release page.

Untick **Check for updates automatically** at the bottom of the window to turn
it off; **Check now** checks right away.

## Building from source

The app is written in C++20 with [Qt 6](https://www.qt.io/) (Widgets and
Multimedia) and built with CMake. Every push is built, tested and packaged on
Windows, Linux and macOS by [GitHub Actions](.github/workflows/build.yml); for a
version tag the packages are uploaded to the release. The workflow shows the
exact steps for each platform: `ci/install_qt.py` installs Qt,
`ci/get_ffmpeg.py` downloads the FFmpeg that gets bundled, and `packaging/`
creates the Linux AppImage and the macOS disk image. The ARM64 versions are
built and tested on GitHub's ARM64 machines, with Qt's and FFmpeg's native ARM64
builds.

On Windows you need:

- Visual Studio 2022 or later with the *Desktop development with C++* workload
- CMake 3.21 or later and Ninja:

  ```bash
  winget install Kitware.CMake Ninja-build.Ninja
  ```

- Qt 6.5 or later for MSVC 64-bit (developed with Qt 6.11.3), with the Qt
  Multimedia module (used by the Editor to play videos). The build script looks
  for it in `C:\Qt\6.11.3\msvc2022_64`; otherwise pass `-QtDir` or set `QTDIR`.
  On Linux and macOS, Qt Multimedia's player also needs the Qt Quick module
  (`qtdeclarative`).
- [FFmpeg](https://ffmpeg.org/) (`ffmpeg.exe` and `ffprobe.exe`), either in an
  `ffmpeg/` folder inside the project or in your `PATH`:

  ```bash
  winget install Gyan.FFmpeg
  ```

- To create the installer, [Inno Setup](https://jrsoftware.org/isinfo.php) 6
  (without it, only the zip is created):

  ```bash
  winget install JRSoftware.InnoSetup
  ```

Then, from PowerShell:

```powershell
.\build.ps1              # build: build\FrameStudio.exe
.\build.ps1 -Test        # build and run the tests
.\build.ps1 -Package     # build and create the installer and the zip in build\
```

The script sets up the Visual Studio compiler environment by itself. To run
`build\FrameStudio.exe` directly, Qt's `bin` folder must be in `PATH`. You can
pass a video as an argument to load it right away:

```powershell
build\FrameStudio.exe "C:\path\to\video.mp4"
```

The package contains `FrameStudio.exe`, only the Qt libraries and plugins it
needs, the Microsoft C++ runtime and FFmpeg. The FFmpeg to bundle is the
`ffmpeg/` folder of the project, copied as it is; `python ci/get_ffmpeg.py
ffmpeg` fills it with the build used for the releases. If that folder doesn't
exist, on Windows the FFmpeg found in `PATH` is used. The same FFmpeg is copied
to `build\ffmpeg`, so the tests use exactly what gets shipped.

The FFmpeg builds bundled with the releases:

| Platform | Build | Contents of `ffmpeg/` |
|----------|-------|-----------------------|
| Windows x64 | [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) *full-shared* | `ffmpeg.exe`, `ffprobe.exe` and their DLLs |
| Windows ARM64 | [BtbN](https://github.com/BtbN/FFmpeg-Builds) *winarm64-gpl-shared* | `ffmpeg.exe`, `ffprobe.exe` and their DLLs |
| macOS | [Martin Riedl](https://ffmpeg.martin-riedl.de/), static, Apple Silicon | `ffmpeg`, `ffprobe` |
| Linux | [BtbN](https://github.com/BtbN/FFmpeg-Builds) *gpl-shared*, x86_64 or ARM64 | `bin/ffmpeg`, `bin/ffprobe`, `lib/` |

On Windows the *full-shared* build has every feature of the *full* build, but
`ffmpeg.exe` and `ffprobe.exe` share one copy of the libraries instead of
containing one each: 230 MB instead of 424 MB.

The app icon is drawn in `resources/icon.svg` (and `icon-small.svg`, a
simplified version for 16–24 px). After editing them, regenerate the icons for
every platform (`icon.ico`, `icon.icns`, `icon.png`), with Qt's `bin` folder in
`PATH`:

```powershell
cmake --build build --target make_icon
build\make_icon.exe resources\icon.svg resources\icon-small.svg resources
```

The tests (`tests/`) generate their own videos with FFmpeg and drive the real
window offscreen: every format, codec and device combination (checking which
encoder actually wrote the file), every bitrate mode of every encoder, portrait
phone videos, cover art, keyframe spacing and closed GOP, the position of the
seek index, the accuracy of the estimate, *Fit quality to limit* and *Fit
bitrate to limit*, the automatic settings, the editing (with the keyboard and
the mouse), playback, export and the copy without re-encoding of edited videos
(checking the order of their frames), and the box shown at the end. GPU
encoders are tested only when the PC has them.

## How the estimate works

1. Four 4-second samples spread across the video (or across what is left of it
   after editing) are encoded with the chosen video settings.
2. Each sample starts with a keyframe, while the real file only has one every
   few seconds: the size of the keyframes the samples have in excess is
   subtracted. Without this, videos with little motion (where keyframes are
   most of the size) would be estimated up to 50% too big.
3. The bytes produced give the average video bitrate, to which the chosen
   audio bitrate and about 1% for the container are added.
4. The estimate updates automatically whenever a setting changes; results that
   were already computed are reused.

For videos longer than 20 seconds the margin of error is roughly ±10%. That
is why *Fit quality to limit* aims for 94% of the limit you set.

With a bitrate (VBR or CBR) nothing needs to be encoded: the size is the video
and audio bitrates × the duration, plus about 1% for the container. Short
samples would be misleading here, because encoders take a few seconds to settle
on a bitrate. Encoders keep to a constant bitrate within a few %; with a
variable one, simple videos (such as screen recordings) come out smaller, and
GPU encoders can end up to about 10% above the average. *Fit bitrate to limit*
aims for 94% of the limit, 90% for GPU encoders with a variable bitrate.

With slow encoders the estimate takes longer, because the samples are really
encoded: H.266/VVC can take a minute or more, while GPU encoders take a few
seconds.

## License

The source code of this app is released under the MIT License. See
[LICENSE](LICENSE).

The packaged app also includes:

- [Qt 6](https://www.qt.io/), used under the LGPLv3. Its license texts are in
  the `licenses` folder of the package; the Qt source code is available at
  [download.qt.io](https://download.qt.io/official_releases/qt/). Qt
  Multimedia plays videos with its own build of the FFmpeg libraries (the
  `avcodec`, `avformat`, `avutil`, `swresample` and `swscale` libraries next to
  the program): FFmpeg 7.1, under the LGPL 2.1 or later, which the LGPLv3 text
  covers. Its source code is at [ffmpeg.org](https://ffmpeg.org/releases/).
- [FFmpeg](https://ffmpeg.org/), licensed under the GPLv3. Its license and
  build information, including where to find the FFmpeg source code, are in the
  `ffmpeg` folder of the package (in the app's `Contents/Resources` on macOS).
