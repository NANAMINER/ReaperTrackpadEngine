# REAPER Trackpad Engine

Experimental macOS extension that replaces REAPER's native trackpad navigation
with smoother, more predictable gestures.

## Features

- Smooth horizontal and vertical scrolling in the Arrange view
- Smooth vertical scrolling Track Control Panel
- Smooth horizontal scrolling in the Mixer
- Adaptive pinch momentum: longer release for fast gestures, shorter for slow ones (the pinch-to-zoom is anchored to REAPER's edit cursor)
- Persistent master switch for the complete gesture engine

## Compatibility

The downloadable build currently supports:

- macOS
- Apple Silicon (`arm64`)
- Native Apple Silicon REAPER
- Apple trackpads and Magic Trackpad

Intel and Universal builds are not available yet.

## Download and install

1. Download
   [`reaper_trackpadengine.dylib`](dist/macos-arm64/reaper_trackpadengine.dylib?raw=1).
2. Fully quit REAPER.
3. Copy the file to: ~/Library/Application Support/REAPER/UserPlugins
5. Restart REAPER.
6. Open the Action List and search for `Trackpad Engine`.
7. Run **Trackpad Engine: Toggle engine (all gestures)** once.

The first installation starts with the engine disabled. Its state is saved and
restored automatically on later launches.

For troubleshooting, updating, and uninstalling, see the full
"INSTALLATION.txt"(dist/macos-arm64/INSTALLATION.txt).

## macOS quarantine

If REAPER does not show the Trackpad Engine actions, macOS may have quarantined
the downloaded file. Only if you trust the download, run:

```bash
xattr -d com.apple.quarantine \
  "$HOME/Library/Application Support/REAPER/UserPlugins/reaper_trackpadengine.dylib"
```

Then fully quit and reopen REAPER.

## Usage

The master action controls horizontal scrolling, vertical scrolling, and pinch
zoom together:

```text
Trackpad Engine: Toggle engine (all gestures)
```

Separate actions remain available for selectively enabling or disabling each
gesture. Diagnostic logging and calibration actions are optional and are not
needed for normal use.

## Build from source

Requirements:

- Xcode Command Line Tools
- CMake 3.20 or newer

```bash
git clone https://github.com/duanebeatzz/ReaperTrackpadEngine.git
cd ReaperTrackpadEngine
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The extension is produced at:

```text
build/reaper_trackpadengine.dylib
```

To install the compiled file:

```bash
./scripts/install.sh
```

REAPER must be fully restarted after installation or updating.

## Architecture

```text
NSEvent / REAPER view
        ↓
TrackpadEvent
        ↓
MotionEngine and gesture processors
        ↓
NavigationCommand
        ↓
ReaperNavigation
        ↓
REAPER API
```

The Objective-C++ layer captures trackpad events and converts them into plain
C++ data. Gesture processors handle motion and momentum, while
`ReaperNavigation` is the only component that applies changes through the
REAPER API.

## Current limitations

- The prebuilt binary is Apple Silicon only.
- Very fast Mixer swipes may still show a small visual glitch.
- Track-height gestures are not implemented.
- There is no settings GUI; motion parameters are currently defined in code.

## Verification

The SHA-256 checksum for the downloadable binary is stored in
[`SHA256SUMS.txt`](dist/macos-arm64/SHA256SUMS.txt).

## License

ReaperTrackpadEngine is released under the [MIT License](LICENSE).

Vendored REAPER SDK and WDL/SWELL headers retain their original license
notices. See [`third_party/NOTICE.md`](third_party/NOTICE.md).
