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

- Forced dark stylesheet (`dark_style.qss` + `style_helper.cpp/h`) **removed**.
- All widgets now inherit `QPalette` from the running TeamSpeak instance, so the soundboard automatically follows the user's active TeamSpeak theme — light, dark, or custom.

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

### Requirements
- Qt 5.15 (msvc2019_64 on Windows, system Qt5 elsewhere)
- Visual Studio 2017+ (Windows) or GCC/Clang
- CMake 3.12+
- FFmpeg headers + libs (system or static under `ffmpeg/lib_*/`)
- 7-Zip (Windows packaging only)

### Quick build (Windows, MSVC + static FFmpeg)
```bat
git clone -b gamebaiters https://github.com/gamebaiters/RP-Soundboard.git
cd RP-Soundboard
build.bat
```
Produces `rp_soundboard_fx_<version>.ts3_plugin` next to `build.bat`.

### Manual CMake (any platform)
```bash
git clone -b gamebaiters https://github.com/gamebaiters/RP-Soundboard.git
cd RP-Soundboard
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
                    -DCMAKE_PREFIX_PATH=/path/to/Qt5 \
                    -DRPSB_MAKE_PLUGIN_FILE=ON
cmake --build build --config Release
```

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
