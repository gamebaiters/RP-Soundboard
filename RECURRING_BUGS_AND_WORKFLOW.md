# Recurring Bugs & Release Workflow

Reference for every new Claude session working on GameBaiters Soundboard.
When context gets compacted, the same bugs tend to resurface because the
fixes live in subtle invariants that aren't obvious from a cold code read.
This doc exists so you (or a future AI session) can skip the diagnosis
phase and go straight to the fix.

---

## 1. Recurring Bug: WAV / PCM Silent Playback

**Symptom:** `.wav` files load but produce silence. MP3/OGG/FLAC work fine.

**Root cause:** PCM containers (WAV, AIFF) set `codecpar->ch_layout` to
`AV_CHANNEL_ORDER_UNSPEC` with only `nb_channels` populated (e.g. 2).
`av_channel_layout_describe()` then emits `"2 channels"` instead of
`"stereo"`. The abuffer source filter accepts it, but the downstream
`aformat=channel_layouts=stereo` filter can't match — every decoded frame
is silently dropped.

**Fix location:** `src/inputfileffmpeg.cpp`, inside `buildFilterGraph()`,
around line 543-560.

**The fix (must be preserved across edits):**

```cpp
// Before calling av_channel_layout_describe on srcLayout:
AVChannelLayout normalized;
bool useNormalized = false;
if (srcLayout && srcLayout->order == AV_CHANNEL_ORDER_UNSPEC
    && srcLayout->nb_channels > 0) {
    av_channel_layout_default(&normalized, srcLayout->nb_channels);
    useNormalized = true;
}
AVChannelLayout *describeLayout = useNormalized ? &normalized : srcLayout;
// Use describeLayout for av_channel_layout_describe, NOT srcLayout.
```

**When it breaks again:** Any time `buildFilterGraph()`'s channel layout
source is refactored, moved, or the variable feeding `av_channel_layout_describe`
changes. The normalization step is easy to accidentally drop.

**Diagnosis shortcut:** Check `rpsb_debug.log` for
`channel_layout=2 channels` — that string instead of `stereo` means the
UNSPEC normalization is missing or bypassed.

---

## 2. Recurring Bug: FFmpeg ABI Mismatch (Stale CMake Cache)

**Symptom:** ALL playback breaks. Log shows impossible codec parameters
(e.g. `sample_rate=1`, `channels=32761`), or the plugin crashes on any
file open.

**Root cause:** The directory `upstream-clone/ffmpeg/build/x86_64/lib/`
contains FFmpeg **n7.x** static libraries (libavcodec MAJOR=61, ~12 MB
archives). The correct libraries live at `ffmpeg-static/lib/` (n6.1.1,
MAJOR=60, ~2.7 MB). CMake's `find_library` caches the first path it finds
in `build_local/CMakeCache.txt`. If `build_local/` is reused without
`-Clean` after the stale n7.x libs appear, the plugin links headers
from n6.1.1 against binaries from n7.x — struct offsets differ, every
field read from `AVCodecParameters` or `AVCodecContext` is garbage.

**Fix:**

```powershell
# Delete the cmake build cache
Remove-Item -Recurse -Force build_local
# Delete the stale n7.x FFmpeg artifacts (if present)
Remove-Item -Recurse -Force upstream-clone\ffmpeg\build -ErrorAction SilentlyContinue
# Rebuild
.\build_local.ps1 -Local
```

Or equivalently: `.\build_local.ps1 -Local -Clean`

**Prevention rule:** ALWAYS use `-Clean` when FFmpeg paths, versions, or
include directories change. Never trust a cached `build_local/CMakeCache.txt`
across FFmpeg modifications.

**Diagnosis shortcut:** Open `build_local/CMakeCache.txt`, search for
`avcodec`. If the path points to `upstream-clone/ffmpeg/build/` instead of
`ffmpeg-static/lib/`, that's the bug. Also: compare archive sizes —
n6.1.1 avcodec.lib is ~2.7 MB; n7.x is ~12 MB.

---

## 3. Recurring Bug: Debug Logging Silently Disabled

**Symptom:** User enables "Write debug log file" checkbox in settings.
No `rpsb_debug.log` is created or written to.

**Root cause:** Both `src/inputfileffmpeg.cpp` and `src/samples.cpp` have
a compile-time `#define RPSB_FILE_DEBUG` gate. If set to `0`, every
`dbgLog()` call compiles to a no-op regardless of the runtime checkbox.

**The rule:** `RPSB_FILE_DEBUG` MUST be `1` in both files. The runtime
flag `g_rpsbLogsEnabled` (toggled by the settings checkbox via
`ConfigModel::setLogsEnabled`) is the sole switch.

**Current locations:**
- `src/inputfileffmpeg.cpp` line ~53: `#define RPSB_FILE_DEBUG 1`
- `src/samples.cpp` line ~28: `#define RPSB_FILE_DEBUG 1`

**When it breaks:** If someone resets the define to 0 "for release" or
during a merge with the upstream repo where it was originally 0.

---

## 4. Codec Parameters: Always Read from codecpar

**Symptom:** Certain containers (M4A, AAC, some MP4) produce garbage
playback or crash. Log shows `sample_rate=1` or `channels=32761`.

**Root cause (when not ABI mismatch):** `avcodec_parameters_to_context()`
can leave `m_codecCtx` with partially-initialized or zero fields for
certain containers. The authoritative source is always
`m_formatCtx->streams[m_audioStream]->codecpar`.

**The rule:** In `buildFilterGraph()` and anywhere sample rate / channel
layout / sample format is read:
- Read `par->sample_rate`, `par->ch_layout`, `par->format` from codecpar
- Use `m_codecCtx` fields only as fallback
- Validate: reject if `rate < 1000`, `channels < 1`, or `channels > 64`

---

## 5. Local Build Workflow (Windows)

**Prerequisites:**
- Visual Studio 2022 (MSVC v143)
- Qt 5.15.2 MSVC 2019 x64 at `D:\QT\Qt64\5.15.2\msvc2019_64` (configurable)
- MSYS2 at `C:\msys64` (for FFmpeg bootstrap only)
- 7-Zip (optional, falls back to Compress-Archive)

**Commands:**

```powershell
# Normal build (reuses cached FFmpeg + cmake state)
.\build_local.ps1 -Local

# Clean cmake cache (use after ANY FFmpeg change)
.\build_local.ps1 -Local -Clean

# Force full FFmpeg rebuild from n6.1.1 source (~20-40 min)
.\build_local.ps1 -Local -RebuildFFmpeg

# Configure only (opens solution in VS)
.\build_local.ps1 -Local -ConfigureOnly
```

**Output:** `SOUNDBOARD_4.0\rp_soundboard_fx_<version>_win64_local.ts3_plugin`

**To test:** Copy the `.ts3_plugin` to the Windows VM, double-click to
install via TS3's package_inst, restart TeamSpeak 3.

**DLL size sanity check:** A correctly linked plugin DLL is >2 MB (FFmpeg
statically linked). If it's <1.8 MB, FFmpeg didn't link — check cmake
output for library paths.

---

## 6. CI Release Workflow (GitHub Actions)

**File:** `.github/workflows/release.yml`

**Trigger:** Push a `v*` tag.

**Steps to cut a release:**

```bash
# 1. Update version files
#    - version.xml: latestVersion (int), latestVersionString, download URL
#    - release-notes.txt: add new section at top

# 2. Commit
git add version.xml release-notes.txt
git commit -m "v2.1.3: <description>"

# 3. Tag and push
git tag v2.1.3
git push origin gamebaiters
git push origin v2.1.3
```

**What CI does:**
1. **build-windows** (windows-latest): Bootstraps static FFmpeg n6.1.1
   via MSYS2+MSVC, builds plugin DLL, uploads artifact
2. **build-linux** (ubuntu-22.04): Uses system FFmpeg/Qt5 packages, builds
   `.so`, uploads artifact
3. **build-macos** (self-hosted Apple Silicon runner): Uses Intel Homebrew
   Qt5/FFmpeg via Rosetta, builds `.dylib` bundle with recursive dep
   bundling + rpath rewrites, also builds `Install_Soundboard_macOS.zip`
   (.app installer)
4. **package-and-release**: Downloads all 3 platform artifacts, packages
   each into separate `.ts3_plugin` zips (platform-isolated — macOS dylibs
   don't leak into Windows package), creates GitHub Release with all assets

**Release assets produced:**
- `rp_soundboard_fx_<ver>_win64.ts3_plugin`
- `rp_soundboard_fx_<ver>_linux_amd64.ts3_plugin`
- `rp_soundboard_fx_<ver>_macos_x86_64.ts3_plugin`
- `Install_Soundboard_macOS.zip`
- `uninstall_soundboard.bat` / `.sh` / `_macos.sh`

---

## 7. Version Files Checklist

Before every release, update these files:

| File | What to change |
|------|----------------|
| `version.xml` | `<latestVersion>` (build int, e.g. 20103), `<latestVersionString>` (e.g. 2.1.3), `<url>` (download URL with new tag) |
| `release-notes.txt` | Add new version section at top |
| `src/package.ini.in` | Only if format changes (version injected by CI via `@version@` placeholder) |

The `version.py` script auto-generates `version.h` from the git tag at
cmake configure time — no manual edit needed there.

---

## 8. macOS Auto-Updater

**How it works:**
- `UpdateChecker.cpp` fetches `version.xml` (single Windows download URL)
- At download time, `askUserForUpdate()` rewrites the URL suffix per platform:
  - `__APPLE__`: `_win64.ts3_plugin` → `_macos_x86_64.ts3_plugin`
  - `__linux__`: `_win64.ts3_plugin` → `_linux_amd64.ts3_plugin`
- `updater_qt.cpp` `executeFile()` generates a platform-specific helper script:
  - **macOS**: kills TS3, removes old plugin variants from `~/Library/Application Support/TeamSpeak 3/plugins/`, extracts via `ditto -x -k`, clears quarantine xattrs, ad-hoc codesigns, relaunches TS3, shows native notification

**What can break:**
- If release asset naming convention changes, the URL rewrite fails silently
- If TS3.app changes its bundle name, the `open -a 'TeamSpeak 3'` relaunch fails
- The helper script assumes `~/Library/Application Support/TeamSpeak 3` as primary path (with fallbacks)

---

## 9. Quick Diagnosis Flowchart

```
ALL playback broken?
  ├─ YES → Check CMakeCache.txt for stale FFmpeg paths (Bug #2)
  │        → Delete build_local/, rebuild with -Clean
  │
  └─ Only WAV/PCM broken?
       ├─ YES → Check log for "2 channels" vs "stereo" (Bug #1)
       │        → Verify UNSPEC normalization in buildFilterGraph()
       │
       └─ Only certain containers (M4A/AAC)?
            → Check codecpar reads vs m_codecCtx reads (Bug #4)
            → If garbage values, likely ABI mismatch (Bug #2)

No debug log file created?
  → Check RPSB_FILE_DEBUG is 1 in both .cpp files (Bug #3)
```

---

## 10. Files Most Commonly Modified

| File | Role | Watch out for |
|------|------|---------------|
| `src/inputfileffmpeg.cpp` | FFmpeg decoder + filter graph | UNSPEC layout, codecpar reads, RPSB_FILE_DEBUG |
| `src/samples.cpp` | Sampler engine | RPSB_FILE_DEBUG, SlotDsp lifecycle |
| `src/modules/main_page_wiring.cpp` | Cross-module logic | Sandbox init order, profile switch persistence |
| `src/modules/channel.cpp` | Per-channel UI | Sandbox button visibility |
| `src/modules/audio_exporter.cpp` | WAV export worker | Parent=nullptr, SlotDsp heap alloc, Paulstretch ring buffer |
| `src/UpdateChecker.cpp` | Auto-updater | Platform URL rewrite |
| `src/updater_qt.cpp` | Install helper scripts | macOS path assumptions, TS3 relaunch |
| `CMakeLists.txt` | Build config | FFMPEG_STATIC flag, find_library hints |
| `version.xml` | Version manifest | Build int, version string, download URL |
| `.github/workflows/release.yml` | CI pipeline | FFmpeg cache key, platform artifact isolation |
