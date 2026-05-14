# GameBaiters Soundboard

A full-featured TeamSpeak 3 soundboard plugin with 14 real-time DSP effects, per-channel audio sandbox, HRTF spatial audio, waveform visualization, macro system, and cross-platform support.

Fork of [MGraefe/RP-Soundboard](https://github.com/MGraefe/RP-Soundboard), rewritten from scratch on top of the original source.

**Download:** https://github.com/gamebaiters/RP-Soundboard/releases/latest

---

## Features

### Audio playback

- **Format support** — MP3, WAV, OGG, FLAC, AAC, WMA, MP4, MKV, AVI, FLV and anything else FFmpeg can decode.
- **Up to 32 simultaneous slots** — each with independent file, volume, FX and DSP sandbox.
- **Loop** toggle per slot.
- **Crop** — per-button start/stop with ms/s/min selectors.
- **Waveform view** — real-time visualization with click-to-seek.
- **Transport** — play, pause, stop, skip +-5s, skip +-10s.
- **Per-button volume modifier** — independent of channel volume.

### Volume control

- **Dual sliders** per channel: Local (what you hear) and Remote (what others hear).
- **Link toggle** to sync both sliders.
- **Global volume** master.

### Real-time FX (per-channel)

| Effect | What it does |
|--------|-------------|
| **Pitch** | Shift pitch independently of speed (0.33x - 3.0x) |
| **Speed** | Change playback rate without altering pitch |
| **Reverb** | Transient room ambience (0 - 100) |
| **Sync toggle** | Lock pitch and speed together for chipmunk/slow-mo |

FX values persist across restarts when "Remember pitch/speed/reverb" is enabled.

### Audio sandbox (14 DSP effects)

Per-channel isolated DSP chain with drag-and-drop pipeline reordering. Every effect has an independent enable toggle. Full state persists across TS3 restarts.

| # | Effect | Description |
|---|--------|-------------|
| 1 | **16-Band EQ** | ISO 2/3-octave, 20 Hz - 16 kHz, +-12 dB per band, automatic makeup gain |
| 2 | **Compressor** | Threshold, ratio, attack, release, soft knee, makeup gain |
| 3 | **Saturator** | 4 modes: soft clip, tube, tape, hard clip. Drive + tone + mix |
| 4 | **Chorus** | Up to 4 voices, rate/depth/delay controls |
| 5 | **Flanger** | Classic jet-sweep with rate, depth, feedback |
| 6 | **Flangus** | Flanger-chorus hybrid. Multi-voice with spread detuning |
| 7 | **Phaser** | Up to 12 all-pass stages, 200 - 4000 Hz sweep |
| 8 | **Delay** | Up to 3 seconds, damping filter, optional ping-pong stereo |
| 9 | **Reverb** | Freeverb-style (8 combs + 4 all-pass), room size, damping, width |
| 10 | **Limiter** | 3 modes: brick-wall limiter, compressor, noise gate |
| 11 | **Bitcrusher** | Bit depth (1-16) + sample rate (500 - 48000 Hz) reduction |
| 12 | **Generation Loss** | Simulates multi-generational MP3 re-encoding degradation |
| 13 | **Paulstretch** | Extreme time-stretch (1x - 50x) with phase randomization |
| 14 | **HRTF Spatial Audio** | Parametric binaural 3D positioning (see below) |

### Spatial audio / HRTF

Parametric head-related transfer function based on the Brown-Duda 1998 structural model with perceptual extensions.

**Modes:**
- **3D Manual** — position a sound in 3D space (X/Y/Z) via the positional pad
- **3D Rotate** — automated circular orbit with configurable RPM, radius, elevation
- **L/R Pan** — simple equal-power stereo balance
- **8D Preset** — preset spatial trajectories

**Parameters:** distance (0.5 - 5m), stereo width (0 - 120 degrees), head sway LFO, spatial mix crossfade.

**Processing chain per ear:** distance gain, ITD delay, head-shadow filter, pinna FIR, shoulder/torso reflections, front/back disambiguation, concha spectral cue, back-tail reverb.

### Paulstretch

Phase-randomized extreme time-stretching (Paul Nasca 2006 algorithm).

- Stretch factor: 1x - 50x
- Window length: 50 - 1000 ms
- FFT size up to 65536 samples
- Independent L/R phase randomization for wide diffuse stereo
- Lock-free atomic GUI/audio thread sync
- Fast priming with boosted initial feed rate

### Pipeline reordering

Drag and drop effects in the sandbox dialog to change processing order. Default chain:

EQ > Compressor > Saturator > Spatial > Chorus > Flanger > Flangus > Phaser > Delay > Reverb > Limiter > Bitcrusher > Mono > GenerationLoss

### Button grid

- Configurable grid: 2-16 rows, 2-16 columns
- 3 button types: audio, macro, special
- Drag-and-drop reordering
- File drop support
- Custom label, background color, background image per button
- Hotkey overlay display
- Real-time search filter

### Channel system

- Per-channel: volume, FX, waveform, transport, sandbox, peak meter
- Editable channel titles
- Peak meter (L/R, cyan normal / red clipping)
- State persistence per channel (survives restarts)

### Profile system

- 4 independent profiles, switchable via buttons or hotkey
- Each profile has its own sound grid, dimensions, macros
- Export/import profiles as JSON

### Macro system

- Freeze all channel states into a single macro button
- Captures: volumes, FX, files, sandbox state for all channels
- "Restore pre-macro" reverts to the state before the macro fired
- Macro buttons show visual decoration

### Audio export

- Export to WAV (48 kHz stereo)
- Bakes the full signal chain: pitch, speed, reverb, all sandbox DSP, paulstretch
- Threaded non-blocking export with progress dialog
- Cancel support

### Preset system

- Save/load EQ presets (named, persistent)
- Save/load full sandbox presets (all effects + pipeline order)

### Theme system

- 3 user-configurable colors: accent, waveform, background
- Contrast slider
- 15+ derived surface colors auto-calculated
- Real-time theme refresh across all widgets
- Scoped to the soundboard (does not leak into the TS3 client)

### Settings

- Earrape protection (local hard-limiter)
- Link volumes toggle
- Remember pitch/speed/reverb
- Restore session on startup
- Hide waveform (compact mode)
- Audio sandbox master toggle
- Audio meter visibility
- Audio export toggle
- Show hotkeys on buttons
- Disable hotkeys toggle
- Grid rows/cols configuration
- Adapt waveform to FX toggle
- Debug log toggle
- Mute locally / mute myself

### Hotkey system

- Per-button hotkey assignment via TS3 SDK
- Global hotkey disable toggle
- Visual hotkey overlay on buttons
- Block list persisted across restarts

### Auto-updater

- Checks for updates on startup
- Platform-specific artifact selection (Windows / Linux / macOS)
- Downloads from GitHub Releases
- macOS: auto-relaunches TS3 after install

### About dialog

- Version info, project links, credits
- Easter egg: progressive sun animation (yellow > orange > red) with blue supernova explosion

---

## Download and install

Latest release: https://github.com/gamebaiters/RP-Soundboard/releases/latest

### Windows
Double-click the downloaded `.ts3_plugin` file, follow the TeamSpeak installer.

### Linux
```bash
LD_LIBRARY_PATH=.:$LD_LIBRARY_PATH ./package_inst <path-to-file.ts3_plugin>
```
Or extract the `.ts3_plugin` (renamed zip) and copy `plugins/*` into `~/.ts3client/plugins/`.

### macOS
Use the included `Install Soundboard.app` from the macOS zip, or manually copy the `plugins/` contents into `~/Library/Application Support/TS3Client/plugins/`.

---

## Uninstall

Standalone uninstallers ship with each release:

- `uninstall_soundboard.bat` (Windows)
- `uninstall_soundboard.sh` (Linux)
- `uninstall_soundboard_macos.sh` (macOS)

They back up the `plugins/` tree, remove all plugin files, sweep logs, and clean `settings.db`.

---

## Build from source

### Option A — CI release (recommended)

Push a `vX.Y.Z` tag and the GitHub Actions workflow builds Windows + Linux + macOS in parallel, fuses the binaries into a single multi-platform `.ts3_plugin`, and publishes the release automatically.

```bash
git switch gamebaiters
git tag v2.2.0
git push origin gamebaiters v2.2.0
```

Workflow: `.github/workflows/release.yml`

Output structure:
```
package.ini
plugins/
    rp_soundboard_fx_win64.dll
    librp_soundboard_fx_linux_amd64.so
    librp_soundboard_fx_mac.dylib
    libav*.dylib  libsw*.dylib        (macOS only)
    rp_soundboard/                     (default sounds)
```

### Option B — Local build

#### Windows (MSVC + Qt 5.15)

Requirements: Visual Studio 2017+, Qt 5.15.2 (msvc2019_64), CMake 3.12+, MSYS2.

```bat
git clone -b gamebaiters https://github.com/gamebaiters/RP-Soundboard.git
cd RP-Soundboard
build.bat
```

Or via CMake directly:
```bat
cmake -G "NMake Makefiles" -B build -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="C:\Qt\5.15.2\msvc2019_64" ^
    -DffmpegLibHint="C:\path\to\ffmpeg\lib" ^
    -DffmpegIncludeDir="C:\path\to\ffmpeg\include"
cmake --build build --config Release
```

#### Linux

```bash
sudo apt-get install -y build-essential cmake pkg-config python3 zip \
    qtbase5-dev qttools5-dev qttools5-dev-tools \
    libavcodec-dev libavformat-dev libavfilter-dev \
    libavutil-dev libswresample-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

#### macOS

```bash
brew install qt@5 ffmpeg cmake
export PATH="$(brew --prefix qt@5)/bin:$PATH"

cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_PREFIX_PATH="$(brew --prefix qt@5)"
cmake --build build -j$(sysctl -n hw.ncpu)
```

x86_64 only — TeamSpeak 3 on macOS is x86_64; Apple Silicon users run it under Rosetta 2.

### Releasing a new version

1. Update `version.xml` (build number + version string + download URL).
2. Update `release-notes.txt` (add new version section at top).
3. Commit and push to `gamebaiters`.
4. `git tag vX.Y.Z && git push origin vX.Y.Z` — CI builds and publishes.

The build number in `version.xml` must be strictly greater than the previous release for the auto-updater to detect it.

---

## Repository layout

```
RP-Soundboard/
├── src/                     C++ / Qt sources
│   ├── dsp/                 DSP effects (EQ, reverb, HRTF, paulstretch, ...)
│   ├── modules/             Modular UI (main page, channels, settings, ...)
│   ├── config_qt.*          Main soundboard window
│   ├── samples.*            Playback engine
│   ├── inputfileffmpeg.cpp  FFmpeg decoder
│   └── ...
├── .github/workflows/       CI release pipeline
├── pluginsdk/               TS3 plugin SDK headers
├── img/                     Icons and UI assets
├── deploy/                  Installer scripts, package template
├── dist/                    Platform uninstallers
├── ffmpeg/                  Vendored FFmpeg submodule
├── version.xml              Auto-updater manifest
├── release-notes.txt        Changelog (all versions)
├── CMakeLists.txt
├── files.cmake
└── version.py               Generates version.h from git tag
```

---

## Branches

| Branch | Purpose |
|--------|---------|
| `gamebaiters` | Main branch. All releases tagged from here. |
| `master` | Mirror of upstream MGraefe/RP-Soundboard. Used for merging upstream changes. |

---

## License

GPL (inherited from upstream). FFmpeg linked under LGPLv2.1 — source available at [ffmpeg.org](https://ffmpeg.org/download.html) tag n6.1.1.
