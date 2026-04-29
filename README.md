# GameBaiters Soundboard

> Fork of [MGraefe/RP-Soundboard](https://github.com/MGraefe/RP-Soundboard) — a TeamSpeak 3 soundboard plugin extended with multi-soundboard playback, real-time audio FX, waveform scrubbing, transport skip controls, earrape protection and a self-hosted update channel.
>
> The upstream README is preserved below for reference. The **What's different in this fork** section explains every deviation.

---

## What's different in this fork

This branch (`gamebaiters`) sits on top of upstream commit `64794fd` (2026-02-28) and adds the features below. Nothing from the original plugin has been removed except the forced dark stylesheet.

### Audio FX (real-time, per playback)

| Feature | What it does |
|---------|--------------|
| **Pitch slider** | Shifts pitch independently of speed, range `0.5x – 2.0x`. |
| **Speed slider** | Changes playback rate without altering pitch, range `0.5x – 2.0x`. |
| **Pitch + Speed slider** | Linked control: drives pitch and speed together for the classic "chipmunk / slow-mo" effect. Includes Sync/Reset toggle. |
| **Reverb slider** | Real-time reverb intensity, range `1.0x – 20.0x`. |
| **Earrape protection** | Local hard-limiter against accidental ear-blasting peaks. **Only affects what *you* hear** — remote listeners always receive the unaltered stream. |
| **Remember pitch / speed / reverb** | Per-button toggle: the slot keeps its last FX values across plugin restarts and config switches. |

### Transport controls

- **Skip ± 5s** — jump 5 seconds forward or backward inside the currently playing sample.
- **Skip ± 10s** — same, larger increment.
- **Real-time waveform view** — live waveform of the playing sample with click-to-seek. Drag to scrub the playhead anywhere in the file.

### Multi-soundboard mode

Up to **5 soundboards play simultaneously** (slots 0–4). Each slot has its own:

- Buttons grid + hotkeys
- Volume (local + remote)
- Pitch / speed / reverb sliders
- Earrape protection toggle
- Remember pitch/speed/reverb toggle
- Per-sound settings (loop, crop, custom FX, custom color)

The per-sound settings dialog has been redesigned so each parameter lives explicitly under the slot it belongs to.

### Theming

- Forced **dark theme** restored (`dark_style.qss` + `style_helper.cpp/h`).
- All plugin dialogs render in a consistent dark palette regardless of the host TeamSpeak theme.

### Update channel

- Migrated from `mgraefe.de` to GitHub. Auto-updater fetches:
  - `https://raw.githubusercontent.com/gamebaiters/RP-Soundboard/gamebaiters/version.xml`
  - `https://raw.githubusercontent.com/gamebaiters/RP-Soundboard/gamebaiters/release-notes.txt`
- New releases are published at https://github.com/gamebaiters/RP-Soundboard/releases — the auto-updater downloads the `.ts3_plugin` asset directly from the release.

### Build system

- Project target renamed to `rp_soundboard_fx`; output binary is `rp_soundboard_fx_win64.dll`.
- New CMake option `FFMPEG_STATIC=ON` for self-contained single-DLL builds (links `*.a` static FFmpeg libs; supports MSVC + MinGW + macOS).
- `cmake_minimum_required(VERSION 3.12.0)` (was 3.19) for older CMake support.
- Per-target FFmpeg lib paths: `ffmpeg/lib_win_x64`, `ffmpeg/lib_win_x86`, `ffmpeg/lib_lin_x64`, `ffmpeg/lib_lin_x86`.

### Branding

- Plugin name in TeamSpeak: **"GameBaiters - Soundboard"**.
- Custom 16/32/64-px icons under `img/`.

---

## Download and install

Latest release: https://github.com/gamebaiters/RP-Soundboard/releases/latest

### Windows
- Double-click the downloaded `rp_soundboard_fx_<version>.ts3_plugin`, follow the TeamSpeak installer.
- Or drag the file onto `package_inst.exe` inside your TeamSpeak installation folder.

### Linux
```bash
# from inside your TeamSpeak install folder
LD_LIBRARY_PATH=.:$LD_LIBRARY_PATH ./package_inst <path-to-rp_soundboard_fx_x.y.z.ts3_plugin>
```
Or extract the `.ts3_plugin` (it's a renamed zip) and copy `plugins/*` into `~/.ts3client/plugins/`.

### macOS
Same as Linux — extract the archive and drop the contents of its `plugins/` folder into `~/Library/Application Support/TS3Client/plugins/` or `~/.ts3client/plugins/`.

---

## Uninstall completely

Standalone uninstallers ship with each release archive (and live next to the build scripts in source). They:

1. **Back up** the entire `plugins/` tree + `settings.db` to `~/Desktop/GameBaiters_Soundboard_Backup_<timestamp>/`
2. Kill the TeamSpeak client (with confirmation prompt)
3. Remove every plugin DLL/`.so`/`.dylib` variant
4. Remove asset folders (`rp_soundboard/`, `rp_soundboard_fx/`, `soundboard/`)
5. Sweep logs (`rpsb_debug.log`, `rp_soundboard*.log`, `soundboard*.log`)
6. Clean `settings.db`: `DELETE FROM Plugins WHERE value LIKE '%rp_soundboard%'` + `VACUUM`
7. Remove leftover config files / config directories

Scripts: `uninstall_soundboard.bat` (Windows), `uninstall_soundboard.sh` (Linux), `uninstall_soundboard_macos.sh` (macOS).

---

## Build from source

The recommended workflow is **GitHub Actions** — push a `vX.Y.Z` tag and
the CI builds Windows + Linux + macOS in parallel, fuses the binaries
into a single multi-platform `.ts3_plugin`, and publishes the GitHub
release. No local Mac or Linux box required.

### Option A — Push a tag, let CI build everything (recommended)

```bash
# from any machine (Windows works fine):
git switch gamebaiters
git pull
# ... commit changes, bump version.py + version.xml + release-notes.txt ...
git tag v1.0.103
git push origin gamebaiters
git push origin v1.0.103
```

Workflow: `.github/workflows/release.yml`. It runs three matrix jobs
(`windows-latest`, `ubuntu-22.04`, `macos-13`), uploads each platform's
binaries as artifacts, then a final job zips them into one
`rp_soundboard_fx_<version>.ts3_plugin` containing:

```
package.ini  (Platforms = win32, win64, linux_amd64, mac)
plugins/
    rp_soundboard_fx_win64.dll          (Windows)
    librp_soundboard_fx_linux_amd64.so  (Linux)
    librp_soundboard_fx_mac.dylib       (macOS x86_64)
    libav*.dylib  libsw*.dylib          (FFmpeg, macOS only)
    rp_soundboard/                      (default sounds)
```

The release page is created automatically at
`https://github.com/gamebaiters/RP-Soundboard/releases/tag/<your-tag>`.

To run the workflow manually without a tag (for testing), use the
"Run workflow" button under the **Actions** tab in GitHub.

### Option B — Build locally (per platform)

#### Windows (native, MSVC + Qt 5.15)

Requirements:
- Visual Studio 2017+ with C++ workload
- Qt 5.15.2 (`msvc2019_64`) — install via the Qt online installer
- CMake 3.12+
- 7-Zip (for packaging the .ts3_plugin)
- Prebuilt FFmpeg libs under `ffmpeg-msvc/lib` and `ffmpeg-msvc/include`
  (download a "shared" build from gyan.dev or compile from source).

Quick build via the bundled batch file:

```bat
git clone -b gamebaiters https://github.com/gamebaiters/RP-Soundboard.git
cd RP-Soundboard
build.bat
```

Produces `rp_soundboard_fx_<version>.ts3_plugin` next to `build.bat`.
The script auto-detects VS 2017/2019/2022 and Qt under `F:\Qt`/`F:\Qt64`.

Manual CMake invocation:

```bat
cmake -G "NMake Makefiles" -B build -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="C:\Qt\5.15.2\msvc2019_64" ^
    -DffmpegLibHint="C:\path\to\ffmpeg\lib" ^
    -DffmpegIncludeDir="C:\path\to\ffmpeg\include"
cmake --build build --config Release
```

#### Linux (native or WSL2 from Windows)

If you only have a Windows machine, install **WSL2** with Ubuntu —
Microsoft Store -> "Ubuntu 22.04". WSL2 gives you a real Linux
filesystem and toolchain that produces native `.so` binaries usable on
any Linux distro:

```bash
# inside WSL2 / native Ubuntu
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake pkg-config python3 zip \
    qtbase5-dev qttools5-dev qttools5-dev-tools \
    libqt5network5 libqt5gui5 libqt5widgets5 libqt5core5a \
    libavcodec-dev libavformat-dev libavfilter-dev \
    libavutil-dev libswresample-dev

git clone -b gamebaiters https://github.com/gamebaiters/RP-Soundboard.git
cd RP-Soundboard
cmake -B build -DCMAKE_BUILD_TYPE=Release \
              -DffmpegLibHint=/usr/lib/x86_64-linux-gnu \
              -DffmpegIncludeDir=/usr/include
cmake --build build -j$(nproc)
```

Output: `build/librp_soundboard_fx_linux_amd64.so` (or similar — name
follows the `_linux_amd64.so` suffix from `CMakeLists.txt`).

To run a Windows -> WSL2 build round-trip you can stay on the Windows
filesystem:

```bash
# in WSL2:
cd /mnt/c/Users/<you>/Desktop/SOUNDBOARD_4.0/upstream-clone
cmake -B build_linux ...
```

#### macOS (native — best path)

The macOS dylib needs Apple's toolchain (clang + Mach-O). The realistic
options are:

1. **Native Mac** — recommended.
2. **GitHub Actions macos-13 runner** (Option A above) — also works
   from Windows because the build runs on Apple-provided hardware.
3. **`osxcross` from WSL2** — possible but fragile; see "Cross-build"
   section below.

On a native Mac:

```bash
brew install qt@5 ffmpeg cmake python git
export PATH="$(brew --prefix qt@5)/bin:$PATH"

git clone -b gamebaiters https://github.com/gamebaiters/RP-Soundboard.git
cd RP-Soundboard
cmake -B build -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_OSX_ARCHITECTURES=x86_64 \
              -DCMAKE_PREFIX_PATH="$(brew --prefix qt@5)" \
              -DffmpegLibHint="$(brew --prefix ffmpeg)/lib" \
              -DffmpegIncludeDir="$(brew --prefix ffmpeg)/include"
cmake --build build -j$(sysctl -n hw.ncpu)

# rpath fixups
DYLIB="$(find build -name 'librp_soundboard_fx*_mac.dylib' | head -1)"
for q in Core Widgets Gui Network; do
    install_name_tool -change "libQt5${q}.dylib" \
        "@rpath/libQt5${q}.dylib" "$DYLIB"
done
```

The output dylib lives next to the build dir; bundle it together with
the FFmpeg dylibs from `$(brew --prefix ffmpeg)/lib/libav*.dylib` and
`libsw*.dylib` into the `.ts3_plugin` zip (TS3 on macOS needs them
adjacent to the plugin). The release CI already does this.

Why x86_64 only: TeamSpeak 3 on macOS is x86_64 itself; loading an
arm64 dylib into an x86_64 process fails. Apple Silicon users run
TeamSpeak 3 under Rosetta 2 and the x86_64 plugin matches.

### Option C — Cross-build for Linux/macOS from Windows (advanced)

For users who want a single Windows machine to produce all three
platform binaries without GitHub Actions:

#### Linux .so via WSL2

Install WSL2 with Ubuntu 22.04 (one-time):

```powershell
# in PowerShell as admin:
wsl --install -d Ubuntu-22.04
```

Then follow the Linux build steps above inside the WSL2 shell. The
resulting `.so` is a real ELF-x86_64 binary, identical to one built on
native Ubuntu.

#### macOS .dylib via osxcross (WSL2 + clang)

This is non-trivial and depends on an Apple SDK tarball that you must
extract from a Mac you have legal access to. Quick outline (full guide
at https://github.com/tpoechtrager/osxcross):

```bash
# inside WSL2 Ubuntu
sudo apt-get install -y clang cmake patch python3 libxml2-dev \
    libssl-dev liblzma-dev libbz2-dev libfuse2 cpio
git clone https://github.com/tpoechtrager/osxcross.git
cd osxcross
# Place MacOSX13.3.sdk.tar.xz (extracted from your Mac's Xcode) in tarballs/
SDK_VERSION=13.3 OSX_VERSION_MIN=10.15 ./build.sh

export PATH="$(pwd)/target/bin:$PATH"
export OSXCROSS_TARGET_DIR="$(pwd)/target"

# Build Qt5 + FFmpeg with the osxcross-clang wrappers (lengthy, see
# the project README). Then point our CMake at them:
cmake -B build_macos \
    -DCMAKE_TOOLCHAIN_FILE="$(pwd)/target/toolchain.cmake" \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_PREFIX_PATH=/path/to/cross-built-qt5 \
    -DffmpegLibHint=/path/to/cross-built-ffmpeg/lib \
    -DffmpegIncludeDir=/path/to/cross-built-ffmpeg/include
cmake --build build_macos
```

Caveats:
- Apple SDK redistribution is restricted; use only an SDK extracted
  from a Mac you own.
- The cross-built dylib will fail to load if Qt and FFmpeg were not
  built with the same SDK / target.
- Code-signing requires `ldid2` (also part of osxcross) for ad-hoc
  signature, but Gatekeeper still warns end users without an Apple
  Developer ID.

For most maintainers, **Option A (GitHub Actions) is strictly easier**
than osxcross — Apple provides macOS runners free for public repos.

### Releasing a new version

1. Bump `TS3SB_VERSION_BUILD` literal in `version.py` (next integer).
2. Update `version.xml` to advertise the new build + version string.
3. Refresh `release-notes.txt` (user-facing, no internal QA notes).
4. Commit + push to `gamebaiters`.
5. `git tag vX.Y.Z && git push origin vX.Y.Z` -> CI builds & releases.

Three numbers must stay coherent: `TS3SB_VERSION_BUILD` (in the binary),
`<latestVersion>` (in `version.xml`), and the git tag. If they drift,
clients enter an offer-update-do-nothing loop.

---

## Repository layout

```
RP-Soundboard/                    (gamebaiters branch)
├── src/                          C++ + Qt sources
│   ├── AudioEffectsDialog.{cpp,h}    pitch/speed/reverb dialog
│   ├── config_qt.{cpp,h,ui}          main soundboard window
│   ├── soundsettings_qt.{cpp,h,ui}   per-sound settings (multi-soundboard aware)
│   ├── UpdateChecker.{cpp,h}         hits version.xml on GitHub
│   ├── samples.{cpp,h}               playback engine (pitch/speed/reverb)
│   └── ...
├── pluginsdk/                    TS3 plugin SDK headers
├── img/                          icons + UI assets
├── deploy/plugins/rp_soundboard/ default sound assets
├── version.xml                   AUTO-UPDATER FEED
├── release-notes.txt             AUTO-UPDATER "details" text
├── CMakeLists.txt
├── files.cmake
└── version.py                    generates src/version/version.h from git tag
```

---

## Branches

| Branch | What it is |
|--------|------------|
| `gamebaiters` | This fork's main branch. All releases are tagged from here. |
| `master` | Mirror of upstream `MGraefe/RP-Soundboard@master`. Kept around so we can `git merge upstream/master` cleanly. Don't commit to it. |

Upstream is configured as the `upstream` remote: `git fetch upstream master` to pull new MGraefe commits.

---

## Contributing / re-using the fork

The fork is GPL-licensed (inherited from upstream). To use it as a base for your own community:

```bash
# 1. fork on GitHub: gamebaiters/RP-Soundboard -> your-user/RP-Soundboard
# 2. clone your fork
git clone -b gamebaiters https://github.com/your-user/RP-Soundboard.git
# 3. edit src/UpdateChecker.cpp - point CHECK_URL at your raw branch URL
# 4. edit version.xml - point <url> at your release download URL
# 5. rebuild and re-publish under your own tags
```

---

## License

Same as upstream (see `LICENSE`). The plugin links FFmpeg under LGPLv2.1 — its corresponding source is available at http://mgraefe.de/rpsb/ffmpeg-source.zip.

---

# ↓ Original upstream README ↓

## RP Soundboard
Easy to use soundboard for Teamspeak 3

A simple yet powerful soundboard that requires no complicated setup! Just install and it's ready to use. No extra tools, no fiddling with push-to-talk settings etc.
It comes with a set of predefined sounds but of course you can choose your own.

### Download and Info
- Official page and downloads: https://www.myteamspeak.com/addons/9e5d66d9-b951-4f46-9b08-0e62909235ee
- Manual downloads: https://github.com/MGraefe/RP-Soundboard/releases

### Features
- Almost any file type (mp3, mp4, wav, flac, ogg, avi, mkv, ...) is supported
- Supports playback of video files (just sound of course, this aint a video player)
- Crop sounds to play only your favorite portion of a sound
- Adjust volume gain for each sound file
- Set keyboard hotkeys for each of your buttons
