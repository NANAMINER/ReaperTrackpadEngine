from pathlib import Path

root = Path('source')

# ---------------------------------------------------------------------------
# v3.4 MIDI-only refinement. v3.2 Arrange path stays untouched.
# ---------------------------------------------------------------------------

# TrackpadEvent: carry MIDI view height so vertical scrollbar logical units can
# be converted to actual on-screen points instead of treating one scrollbar
# unit as one pixel (which is false for REAPER's MIDI vertical scrollbar).
p = root / 'src/TrackpadEvent.h'
s = p.read_text()
needle = '''    double viewWidthPixels = 0.0;\n'''
repl = needle + '''    double viewHeightPixels = 0.0;\n'''
if needle not in s:
    raise SystemExit('TrackpadEvent viewWidth marker not found')
s = s.replace(needle, repl, 1)
p.write_text(s)

# Capture the height from the same exact geometry view already used by v3.3
# (midiview=1001 in MIDI, Arrange=1000 otherwise).
p = root / 'src/ViewSwizzleInterceptor.mm'
s = p.read_text()
needle = '''    te.viewWidthPixels = geometryView.bounds.size.width;\n'''
repl = needle + '''    te.viewHeightPixels = geometryView.bounds.size.height;\n'''
if needle not in s:
    raise SystemExit('geometry width marker not found')
s = s.replace(needle, repl, 1)
p.write_text(s)

p = root / 'src/ReaperNavigation.cpp'
s = p.read_text()

# Resolve the actual keyboard section. KbdSectionInfo::onAction is the REAPER
# SDK path that accepts val/valhw/relmode, unlike MIDIEditor_OnCommand which is
# necessarily a discrete command invocation.
needle = '''using MIDIEditorGetActiveFn = HWND (*)();\nusing MIDIEditorOnCommandFn = bool (*)(HWND, int);\n\nReaperGetFunc g_reaperGetFunc = nullptr;\nJSGetScrollInfoFn g_jsGetScrollInfo = nullptr;\nJSSetScrollPosFn g_jsSetScrollPos = nullptr;\nMIDIEditorGetActiveFn g_midiEditorGetActive = nullptr;\nMIDIEditorOnCommandFn g_midiEditorOnCommand = nullptr;\n'''
repl = '''using MIDIEditorGetActiveFn = HWND (*)();\nusing MIDIEditorOnCommandFn = bool (*)(HWND, int);\nusing SectionFromUniqueIDFn = KbdSectionInfo *(*)(int);\n\nReaperGetFunc g_reaperGetFunc = nullptr;\nJSGetScrollInfoFn g_jsGetScrollInfo = nullptr;\nJSSetScrollPosFn g_jsSetScrollPos = nullptr;\nMIDIEditorGetActiveFn g_midiEditorGetActive = nullptr;\nMIDIEditorOnCommandFn g_midiEditorOnCommand = nullptr;\nSectionFromUniqueIDFn g_sectionFromUniqueID = nullptr;\n'''
if needle not in s:
    raise SystemExit('v3.3 MIDI resolver declarations not found')
s = s.replace(needle, repl, 1)

# Deferred anchor correction. MIDI zoom actions can update scrollbar metrics
# after onAction/MIDIEditor_OnCommand returns, so the v3.3 immediate readback
# could simply see the old page and do no useful correction.
needle = '''uintptr_t g_midiGestureViewId = 0;\n'''
repl = needle + '''\nstruct MidiPendingAnchor {\n    bool active = false;\n    uintptr_t viewId = 0;\n    double logicalAnchor = 0.0;\n    double fraction = 0.5;\n};\nMidiPendingAnchor g_midiPendingAnchor;\n'''
if needle not in s:
    raise SystemExit('midi gesture state marker not found')
s = s.replace(needle, repl, 1)

needle = '''    if (!g_midiEditorOnCommand) {\n        g_midiEditorOnCommand = reinterpret_cast<MIDIEditorOnCommandFn>(\n            g_reaperGetFunc("MIDIEditor_OnCommand"));\n    }\n}\n'''
repl = '''    if (!g_midiEditorOnCommand) {\n        g_midiEditorOnCommand = reinterpret_cast<MIDIEditorOnCommandFn>(\n            g_reaperGetFunc("MIDIEditor_OnCommand"));\n    }\n    if (!g_sectionFromUniqueID) {\n        g_sectionFromUniqueID = reinterpret_cast<SectionFromUniqueIDFn>(\n            g_reaperGetFunc("SectionFromUniqueID"));\n    }\n}\n'''
if needle not in s:
    raise SystemExit('resolver end marker not found')
s = s.replace(needle, repl, 1)

# Replace MIDI pixel axis implementation. Horizontal behavior is kept at the
# same scale. Vertical now converts the scrollbar's logical page span into
# logical-units-per-visible-point. This is the missing conversion behind the
# v3.3 vertical stick/jump behavior.
old = r'''bool ApplyMidiPixelAxis(const TrackpadEvent &event, const char *axis,
                        double delta, double &remainder) {
    ResolvePixelScrollApi();
    if (!g_jsGetScrollInfo || !g_jsSetScrollPos || event.midiViewId == 0)
        return false;

    remainder += delta;
    int pixels = static_cast<int>(remainder);
    if (pixels == 0) return true; // consume while fractional pixels accumulate
    remainder -= static_cast<double>(pixels);

    void *view = reinterpret_cast<void *>(event.midiViewId);
    int pos = 0, page = 0, minPos = 0, maxPos = 0;
    if (!ReadMidiScrollInfo(view, axis, pos, page, minPos, maxPos)) return false;

    // Same natural-scroll convention as the now-confirmed Arrange v3.2 path:
    // content follows the fingers, so viewport position moves opposite delta.
    int target = ClampScrollPosition(static_cast<long long>(pos) - pixels,
                                     page, minPos, maxPos);
    if (target == pos) return true; // boundary: consume, don't invoke stock zoom
    return g_jsSetScrollPos(view, axis, target);
}
'''
new = r'''bool ApplyMidiPixelAxis(const TrackpadEvent &event, const char *axis,
                        double delta, double &remainder) {
    ResolvePixelScrollApi();
    if (!g_jsGetScrollInfo || !g_jsSetScrollPos || event.midiViewId == 0)
        return false;

    void *view = reinterpret_cast<void *>(event.midiViewId);
    int pos = 0, page = 0, minPos = 0, maxPos = 0;
    if (!ReadMidiScrollInfo(view, axis, pos, page, minPos, maxPos)) return false;

    double logicalDelta = delta;
    if (axis && axis[0] == 'v' && event.viewHeightPixels > 1.0 && page > 0) {
        // SCROLLINFO::nPage is expressed in the scrollbar's own logical units.
        // Divide by the actually visible midiview height to map an Apple
        // precise scrolling delta (points) to those units. Keep a broad
        // defensive clamp for malformed/unexpected scrollbar metadata.
        double unitsPerPoint = static_cast<double>(page) / event.viewHeightPixels;
        if (unitsPerPoint < 0.05) unitsPerPoint = 0.05;
        if (unitsPerPoint > 512.0) unitsPerPoint = 512.0;
        logicalDelta *= unitsPerPoint;
    }

    remainder += logicalDelta;
    int units = static_cast<int>(remainder);
    if (units == 0) return true;
    remainder -= static_cast<double>(units);

    // Same natural-scroll convention as the confirmed Arrange v3.2 path:
    // content follows the fingers, so viewport position moves opposite delta.
    int target = ClampScrollPosition(static_cast<long long>(pos) - units,
                                     page, minPos, maxPos);
    if (target == pos) return true;
    return g_jsSetScrollPos(view, axis, target);
}
'''
if old not in s:
    raise SystemExit('ApplyMidiPixelAxis v3.3 block not found')
s = s.replace(old, new, 1)

# Helpers for true relative MIDI actions. REAPER's SDK exposes KbdSectionInfo
# onAction(cmd,val,valhw,relmode,hwnd), which is what wheel/CC actions use.
marker = '''bool ApplyMidiHorizontalPinch(const TrackpadEvent &event, double zoomAmount) {\n'''
helpers = r'''KbdSectionInfo *GetMidiActionSection() {
    ResolvePixelScrollApi();
    return g_sectionFromUniqueID ? g_sectionFromUniqueID(32060) : nullptr;
}

bool MidiSectionHasCommand(KbdSectionInfo *section, int commandId) {
    if (!section || !section->action_list || section->action_list_cnt <= 0)
        return false;
    for (int i = 0; i < section->action_list_cnt; ++i) {
        if (static_cast<int>(section->action_list[i].cmd) == commandId) return true;
    }
    return false;
}

bool DispatchMidiRelativeAction(int commandId, bool positive, HWND editor) {
    KbdSectionInfo *section = GetMidiActionSection();
    if (!section || !section->onAction || !MidiSectionHasCommand(section, commandId))
        return false;

    // Relative mode 1 is REAPER's standard signed relative CC mode:
    // +1 is encoded as 1, -1 as 127. valhw=-1 denotes a 7-bit MIDI-CC style
    // value. This reaches the continuous/relative action implementation
    // rather than the fixed-size 1011/1012 keyboard zoom actions.
    int val = positive ? 1 : 127;
    return section->onAction(commandId, val, -1, 1, editor);
}

bool ApplyPendingMidiAnchor() {
    if (!g_midiPendingAnchor.active) return false;
    ResolvePixelScrollApi();
    if (!g_jsGetScrollInfo || !g_jsSetScrollPos || !g_midiPendingAnchor.viewId) {
        g_midiPendingAnchor.active = false;
        return false;
    }

    void *view = reinterpret_cast<void *>(g_midiPendingAnchor.viewId);
    int pos = 0, page = 0, minPos = 0, maxPos = 0;
    if (!ReadMidiScrollInfo(view, "h", pos, page, minPos, maxPos)) return false;

    long long target = static_cast<long long>(std::llround(
        g_midiPendingAnchor.logicalAnchor -
        g_midiPendingAnchor.fraction * static_cast<double>(page)));
    int clamped = ClampScrollPosition(target, page, minPos, maxPos);
    if (clamped != pos) g_jsSetScrollPos(view, "h", clamped);
    g_midiPendingAnchor.active = false;
    return true;
}

'''
if marker not in s:
    raise SystemExit('ApplyMidiHorizontalPinch marker not found')
s = s.replace(marker, helpers + marker, 1)

# Replace fixed keyboard zoom actions + immediate page correction with one true
# relative action per Cocoa/timer callback and deferred anchor correction.
start = s.index('bool ApplyMidiHorizontalPinch(const TrackpadEvent &event, double zoomAmount) {')
end = s.index('\nbool ApplyMidiCommandVerticalZoom(const TrackpadEvent &event) {', start)
old_block = s[start:end]
new_block = r'''bool ApplyMidiHorizontalPinch(const TrackpadEvent &event, double zoomAmount) {
    if (!event.isInMidiEditor || event.midiViewId == 0 || zoomAmount == 0.0)
        return false;

    ResolvePixelScrollApi();
    if (!g_midiEditorGetActive || event.viewWidthPixels <= 0.0) return false;
    HWND editor = g_midiEditorGetActive();
    if (!editor) return false;

    // Complete the previous action's anchor correction only after REAPER had a
    // UI turn to publish its new scrollbar metrics. v3.3 read them too early.
    ApplyPendingMidiAnchor();

    if (event.phase & (PhaseMayBegin | PhaseBegan)) g_midiPinchAccumulator = 0.0;
    g_midiPinchAccumulator += zoomAmount;

    // One relative zoom quantum per event/timer tick avoids v3.3's bursts of
    // up to eight visibly discrete 1011/1012 commands in a single frame.
    constexpr double kZoomQuantum = 0.010;
    if (std::fabs(g_midiPinchAccumulator) < kZoomQuantum) return true;

    bool zoomIn = g_midiPinchAccumulator > 0.0;
    double sign = zoomIn ? 1.0 : -1.0;

    double fraction = event.localX / event.viewWidthPixels;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;

    void *view = reinterpret_cast<void *>(event.midiViewId);
    int pos = 0, page = 0, minPos = 0, maxPos = 0;
    bool haveAnchor = ReadMidiScrollInfo(view, "h", pos, page, minPos, maxPos);
    double logicalAnchor = static_cast<double>(pos) + fraction * page;

    // 990 = View: Zoom horizontally (MIDI CC relative/mousewheel).
    // Prefer the relative action path; only fall back to the old fixed command
    // when the running REAPER build does not expose command 990 in section
    // 32060 at all.
    bool changed = DispatchMidiRelativeAction(990, zoomIn, editor);
    if (!changed && g_midiEditorOnCommand) {
        changed = g_midiEditorOnCommand(editor, zoomIn ? 1012 : 1011);
    }
    if (!changed) return false;

    g_midiPinchAccumulator -= sign * kZoomQuantum;

    if (haveAnchor) {
        g_midiPendingAnchor.active = true;
        g_midiPendingAnchor.viewId = event.midiViewId;
        g_midiPendingAnchor.logicalAnchor = logicalAnchor;
        g_midiPendingAnchor.fraction = fraction;
    }
    return true;
}
'''
s = s[:start] + new_block + s[end:]

# Replace Cmd+vertical fixed action bursts with the native relative vertical
# zoom action where available. This is not the user's reported pinch bug, but
# keeps all MIDI zoom paths from reintroducing the same stepping architecture.
start = s.index('bool ApplyMidiCommandVerticalZoom(const TrackpadEvent &event) {')
end = s.index('\n\n// Pinch-zoom, anchored on REAPER', start)
old_vz = s[start:end]
new_vz = r'''bool ApplyMidiCommandVerticalZoom(const TrackpadEvent &event) {
    if (!event.isInMidiEditor || event.type != TrackpadEventType::Scroll ||
        !event.hasPreciseDeltas || event.modifiers != ModCommand) {
        return false;
    }

    double x = std::fabs(event.preciseDeltaX);
    double y = std::fabs(event.preciseDeltaY);
    if (y <= x * 3.0) return false;

    ResolvePixelScrollApi();
    if (!g_midiEditorGetActive) return false;
    HWND editor = g_midiEditorGetActive();
    if (!editor) return false;

    if (event.phase & (PhaseMayBegin | PhaseBegan))
        g_midiVerticalZoomAccumulator = 0.0;
    g_midiVerticalZoomAccumulator += event.preciseDeltaY;

    constexpr double kVerticalZoomQuantum = 3.0;
    if (std::fabs(g_midiVerticalZoomAccumulator) < kVerticalZoomQuantum)
        return true;

    bool zoomIn = g_midiVerticalZoomAccumulator > 0.0;
    double sign = zoomIn ? 1.0 : -1.0;

    // 991 = View: Zoom vertically (MIDI CC relative/mousewheel).
    bool changed = DispatchMidiRelativeAction(991, zoomIn, editor);
    if (!changed && g_midiEditorOnCommand) {
        changed = g_midiEditorOnCommand(editor, zoomIn ? 40111 : 40112);
    }
    if (!changed) return false;

    g_midiVerticalZoomAccumulator -= sign * kVerticalZoomQuantum;
    return true;
}
'''
s = s[:start] + new_vz + s[end:]

p.write_text(s)
print('v3.4 MIDI smooth refinement applied')
