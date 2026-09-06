# Changelog

## v3.6 MIDI — 2026-09-05

- Added MIDI-editor-specific precise trackpad panning.
- Routed pinch zoom through one native REAPER MIDI zoom path.
- Temporarily set REAPER’s MIDI horizontal zoom centre to the mouse cursor while the extension is loaded; the prior value is restored on unload.
- Removed the old deferred viewport-anchor correction, which could visibly fight REAPER’s own later update.
- Reduced pinch quantum from `0.035` to `0.060` for a less aggressive response.
- Kept the established Arrange-view v3.2 behaviour unchanged.

### Known limitation

MIDI vertical movement remains quantized to REAPER’s native piano-roll note rows. `CFGEDITVIEW` stores its vertical position and zoom as integers, so even the stock scrollbar moves in row-sized increments.
