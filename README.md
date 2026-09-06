# REAPER Trackpad Engine — MIDI smooth scrolling build

Native macOS extension that makes a MacBook trackpad substantially more natural in REAPER’s Arrange view and MIDI editor.

This repository is a downstream fork of [duanebeatzz/ReaperTrackpadEngine](https://github.com/duanebeatzz/ReaperTrackpadEngine). It preserves the upstream source history and publishes the tested Apple Silicon release builds made for this fork.

## What v3.6 MIDI changes

- Two-finger scrolling pans the Arrange view as in the established Trackpad Engine build.
- Inside the piano roll, two-finger horizontal and vertical scrolling are handled as precise viewport motion.
- Pinch-to-zoom in the MIDI editor is anchored at the mouse cursor instead of REAPER’s edit/play cursor.
- MIDI pinch sensitivity is reduced relative to the preceding experimental build.
- The extension restores REAPER’s previous horizontal-zoom preference when it unloads.

REAPER’s MIDI editor still scrolls vertically in native note-row steps. That is a REAPER viewport limitation; this build does not add a fake visual interpolation layer.

## Requirements

- macOS on Apple Silicon (M1/M2/M3/M4 and later)
- REAPER for macOS
- [ReaPack](https://reapack.com/) and the `js_ReaScriptAPI` extension, installed through ReaPack

`js_ReaScriptAPI` is required for the smooth MIDI viewport scrolling. If it is absent, the extension intentionally leaves MIDI input to stock REAPER rather than interfering with it.

## Install

1. In the [latest release](../../releases/latest), download `reaper-trackpadengine-v3.6-midi-arm64.zip`.
2. Quit REAPER completely (`⌘Q`), not merely close its window.
3. Unzip the download and copy `reaper_trackpadengine.dylib` to:

   ```
   ~/Library/Application Support/REAPER/UserPlugins/
   ```

4. If Finder asks, replace the older file of the same name.
5. Start REAPER again.

## Install js_ReaScriptAPI

1. Install ReaPack if it is not already present, then restart REAPER.
2. Choose `Extensions → ReaPack → Browse packages`.
3. Search for `js_ReaScriptAPI`, install it, then restart REAPER once more.

## Update / remove

- **Update:** quit REAPER, replace the same `.dylib`, start REAPER again.
- **Remove:** quit REAPER and delete `reaper_trackpadengine.dylib` from the `UserPlugins` folder. REAPER will return to its normal trackpad behaviour.

## Verify the download

The release includes `SHA256SUMS.txt`. In Terminal, from the unzipped release folder:

```bash
shasum -a 256 reaper_trackpadengine.dylib
```

For v3.6 MIDI the expected hash is:

```
cee7dab6c28c76646e54c5a228afe7d04270d7ab328aa0d8120c9b193f74b65c
```

## Compatibility and scope

This first downstream release is specifically built for `arm64` macOS. It is not an Intel build and has not been tested on Windows or Linux. It uses REAPER internals and js_ReaScriptAPI, so test it on a copy of an important project before relying on it in a critical session.

The project is not affiliated with Cockos.

## Credits

Upstream project: [duanebeatzz/ReaperTrackpadEngine](https://github.com/duanebeatzz/ReaperTrackpadEngine).

This fork’s MIDI scrolling and cursor-anchored pinch work were developed as a downstream modification. See [CHANGELOG.md](CHANGELOG.md) for the release-specific behaviour.
