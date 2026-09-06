# Downstream v3.6 patch series

The upstream source remains intact in this fork. The files in this directory are the exact downstream patch scripts used to produce the published `v3.6-midi-arm64` binary.

They are intentionally kept as scripts rather than a rewritten snapshot, so every downstream change is reviewable against its upstream context.

## Apply order

1. The Arrange-view patch embedded in `original-arrange-build-workflow.yml`.
2. `reaper_trackpad_safe_v3_patch.py`
3. `reaper_trackpad_v31_arrange_view_fix.py`
4. `reaper_trackpad_v32_js_vertical_fix.py`
5. `reaper_trackpad_v33_midi_integrated.py`
6. `reaper_trackpad_v34_midi_smooth.py`
7. `reaper_trackpad_v35_midi_stability.py`
8. `reaper_trackpad_v36_native_mouse_anchor.py`

Each script expects to be run from a working directory with the upstream repository checked out at `./source`. The original macOS build workflow, including its compiler options, is retained beside the patch scripts as a reproducibility reference.

The release binary was built for arm64 with macOS 11.0 as the deployment target and code-signed ad hoc.
