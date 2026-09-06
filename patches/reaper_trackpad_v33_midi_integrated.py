from pathlib import Path

root = Path('source')

# ---------------------------------------------------------------------------
# TrackpadEvent: carry exact MIDI notes-view identity/context downstream.
# SWS identifies the main MIDI note grid as midi_notesView = 0x3E9 = 1001.
# ---------------------------------------------------------------------------
p = root / 'src/TrackpadEvent.h'
s = p.read_text()
needle = '''    uintptr_t arrangeViewId = 0;\n'''
insert = needle + '''\n    // Actual MIDI note-grid SWELL HWND/NSView (midiview, control ID 1001),\n    // captured from the receiver's ancestor chain. Never guessed globally.\n    uintptr_t midiViewId = 0;\n    bool isInMidiEditor = false;\n'''
if needle not in s:
    raise SystemExit('TrackpadEvent arrangeViewId marker not found')
s = s.replace(needle, insert, 1)
p.write_text(s)

# ---------------------------------------------------------------------------
# ViewSwizzleInterceptor: recognize MIDI only from the real receiver hierarchy,
# use MIDI geometry for mouse anchor, and expose a targeted refresh function so
# MIDI editor classes created after REAPER startup can be swizzled safely.
# ---------------------------------------------------------------------------
p = root / 'src/ViewSwizzleInterceptor.mm'
s = p.read_text()

needle = '''constexpr NSInteger kArrangeViewTag = 1000;\n'''
repl = needle + '''// SWS BR_Util.cpp: midi_notesView = 0x000003E9 (1001).\n// This is the scrolling/zooming note grid, not the piano-key strip (1003).\nconstexpr NSInteger kMidiNotesViewTag = 1001;\n'''
if needle not in s:
    raise SystemExit('Arrange tag marker not found')
s = s.replace(needle, repl, 1)

needle = '''NSView *FindArrangeViewInHierarchy(NSView *view) {\n    for (NSView *candidate = view; candidate; candidate = candidate.superview) {\n        if (candidate.tag == kArrangeViewTag) return candidate;\n    }\n    return nil;\n}\n'''
repl = needle + '''\nNSView *FindMidiNotesViewInHierarchy(NSView *view) {\n    for (NSView *candidate = view; candidate; candidate = candidate.superview) {\n        if (candidate.tag == kMidiNotesViewTag) return candidate;\n    }\n    return nil;\n}\n'''
if needle not in s:
    raise SystemExit('FindArrangeViewInHierarchy block not found')
s = s.replace(needle, repl, 1)

needle = '''    NSView *arrangeAncestor = FindArrangeViewInHierarchy(view);\n    NSView *arrangeView = arrangeAncestor ?: FindViewWithTag(gMainView, kArrangeViewTag);\n    te.arrangeViewId = (uintptr_t)(__bridge void *)arrangeView;\n    te.isInArrangeView = arrangeAncestor != nil;\n'''
repl = '''    NSView *arrangeAncestor = FindArrangeViewInHierarchy(view);\n    NSView *arrangeView = arrangeAncestor ?: FindViewWithTag(gMainView, kArrangeViewTag);\n    NSView *midiNotesView = FindMidiNotesViewInHierarchy(view);\n    te.arrangeViewId = (uintptr_t)(__bridge void *)arrangeView;\n    te.midiViewId = (uintptr_t)(__bridge void *)midiNotesView;\n    te.isInMidiEditor = midiNotesView != nil;\n    te.isInArrangeView = arrangeAncestor != nil;\n'''
if needle not in s:
    raise SystemExit('FillExecutionContext v3.1 marker not found')
s = s.replace(needle, repl, 1)

needle = '''    NSView *geometryView = arrangeView ?: view;\n'''
repl = '''    NSView *geometryView = midiNotesView ?: (arrangeView ?: view);\n'''
if needle not in s:
    raise SystemExit('geometryView marker not found')
s = s.replace(needle, repl, 1)

needle = '''    Diagnostics::Trace("TrackpadTarget",\n        pipelineEvent.isInArrangeView ? "view: arrange" :\n        (pipelineEvent.isInTrackControlPanel ? "view: tcp" : "view: not arrange"));\n'''
repl = '''    Diagnostics::Trace("TrackpadTarget",\n        pipelineEvent.isInMidiEditor ? "view: midi" :\n        (pipelineEvent.isInArrangeView ? "view: arrange" :\n         (pipelineEvent.isInTrackControlPanel ? "view: tcp" : "view: other")));\n'''
if needle not in s:
    raise SystemExit('TrackpadTarget trace marker not found')
s = s.replace(needle, repl, 1)

needle = '''bool Install(void *mainHwnd) {\n'''
# RefreshWindow is inserted after Install's complete body, not before it.
install_end = '''    gInstalled = true;\n    return true;\n}\n\nvoid Remove() {\n'''
repl = '''    gInstalled = true;\n    return true;\n}\n\nvoid RefreshWindow(void *hwnd) {\n    if (!gInstalled || !hwnd) return;\n    NSView *root = (__bridge NSView *)hwnd;\n    if (![root isKindOfClass:[NSView class]]) return;\n\n    // Targeted scan only when an actual MIDI editor is active. Existing\n    // classes are skipped by gRecords; newly-created SWELL view classes are\n    // swizzled once. No global polling of every Cocoa window.\n    NSMutableSet<NSValue *> *seenClasses = [NSMutableSet set];\n    int scrollHooked = 0, magnifyHooked = 0;\n    WalkAndSwizzle(root, seenClasses, 0, &scrollHooked, &magnifyHooked);\n}\n\nvoid Remove() {\n'''
if install_end not in s:
    raise SystemExit('Install end marker not found')
s = s.replace(install_end, repl, 1)
p.write_text(s)

p = root / 'src/ViewSwizzleInterceptor.h'
s = p.read_text()
needle = '''bool Install(void *mainHwnd);\n'''
repl = needle + '''\n// Scan one already-known REAPER/SWELL window for view classes that may have\n// been created after startup (notably the MIDI editor). Existing swizzles are\n// deduplicated. Must be called on REAPER's main/UI thread.\nvoid RefreshWindow(void *hwnd);\n'''
if needle not in s:
    raise SystemExit('ViewSwizzleInterceptor.h Install marker not found')
s = s.replace(needle, repl, 1)
p.write_text(s)

# ---------------------------------------------------------------------------
# ReaperNavigation: MIDI pixel X/Y pan via the same public js_ReaScriptAPI that
# is proven working in v3.2, plus MIDIEditor_OnCommand for zoom. Arrange code
# remains unchanged; MIDI is an early, explicitly-scoped branch.
# ---------------------------------------------------------------------------
p = root / 'src/ReaperNavigation.cpp'
s = p.read_text()

needle = '''using JSSetScrollPosFn = bool (*)(void *, const char *, int);\n\nReaperGetFunc g_reaperGetFunc = nullptr;\nJSGetScrollInfoFn g_jsGetScrollInfo = nullptr;\nJSSetScrollPosFn g_jsSetScrollPos = nullptr;\n'''
repl = '''using JSSetScrollPosFn = bool (*)(void *, const char *, int);\nusing MIDIEditorGetActiveFn = HWND (*)();\nusing MIDIEditorOnCommandFn = bool (*)(HWND, int);\n\nReaperGetFunc g_reaperGetFunc = nullptr;\nJSGetScrollInfoFn g_jsGetScrollInfo = nullptr;\nJSSetScrollPosFn g_jsSetScrollPos = nullptr;\nMIDIEditorGetActiveFn g_midiEditorGetActive = nullptr;\nMIDIEditorOnCommandFn g_midiEditorOnCommand = nullptr;\n\n// MIDI gesture-local fractional state. js_ReaScriptAPI scrollbar positions\n// are integer pixels/units, while Apple trackpad deltas are fractional.\ndouble g_midiHorizontalRemainder = 0.0;\ndouble g_midiVerticalRemainder = 0.0;\ndouble g_midiPinchAccumulator = 0.0;\ndouble g_midiVerticalZoomAccumulator = 0.0;\nuintptr_t g_midiGestureViewId = 0;\n'''
if needle not in s:
    raise SystemExit('v3.2 resolver declarations marker not found')
s = s.replace(needle, repl, 1)

needle = '''    if (!g_jsSetScrollPos) {\n        g_jsSetScrollPos = reinterpret_cast<JSSetScrollPosFn>(\n            g_reaperGetFunc("JS_Window_SetScrollPos"));\n    }\n}\n'''
repl = '''    if (!g_jsSetScrollPos) {\n        g_jsSetScrollPos = reinterpret_cast<JSSetScrollPosFn>(\n            g_reaperGetFunc("JS_Window_SetScrollPos"));\n    }\n    if (!g_midiEditorGetActive) {\n        g_midiEditorGetActive = reinterpret_cast<MIDIEditorGetActiveFn>(\n            g_reaperGetFunc("MIDIEditor_GetActive"));\n    }\n    if (!g_midiEditorOnCommand) {\n        g_midiEditorOnCommand = reinterpret_cast<MIDIEditorOnCommandFn>(\n            g_reaperGetFunc("MIDIEditor_OnCommand"));\n    }\n}\n'''
if needle not in s:
    raise SystemExit('ResolvePixelScrollApi end marker not found')
s = s.replace(needle, repl, 1)

# Insert MIDI helpers immediately before the existing Arrange pinch helper.
marker = '''// Pinch-zoom, anchored on REAPER's edit cursor: read the current visible\n'''
helpers = r'''// -----------------------------------------------------------------------
// MIDI editor navigation
// -----------------------------------------------------------------------

bool ReadMidiScrollInfo(void *view, const char *axis,
                        int &pos, int &page, int &minPos, int &maxPos) {
    ResolvePixelScrollApi();
    if (!g_jsGetScrollInfo || !view) return false;
    int trackPos = 0;
    return g_jsGetScrollInfo(view, axis, &pos, &page, &minPos, &maxPos, &trackPos);
}

int ClampScrollPosition(long long target, int page, int minPos, int maxPos) {
    long long effectiveMax = static_cast<long long>(maxPos) -
                             static_cast<long long>(page > 0 ? page - 1 : 0);
    if (effectiveMax < minPos) effectiveMax = minPos;
    if (target < minPos) target = minPos;
    if (target > effectiveMax) target = effectiveMax;
    return static_cast<int>(target);
}

bool ApplyMidiPixelAxis(const TrackpadEvent &event, const char *axis,
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

void ResetMidiPanStateIfNeeded(const TrackpadEvent &event) {
    if (g_midiGestureViewId != event.midiViewId ||
        (event.phase & (PhaseMayBegin | PhaseBegan))) {
        g_midiGestureViewId = event.midiViewId;
        g_midiHorizontalRemainder = 0.0;
        g_midiVerticalRemainder = 0.0;
    }
}

bool ApplyMidiPan(const TrackpadEvent &event) {
    if (!event.isInMidiEditor || event.midiViewId == 0 ||
        event.type != TrackpadEventType::Scroll || !event.hasPreciseDeltas ||
        event.modifiers != 0) {
        return false;
    }

    ResetMidiPanStateIfNeeded(event);
    if (event.phase & PhaseCancelled) return true;

    double x = std::fabs(event.preciseDeltaX);
    double y = std::fabs(event.preciseDeltaY);
    if (x == 0.0 && y == 0.0) return true;

    constexpr double kDominanceRatio = 3.0;
    bool wantHorizontal = x > y * kDominanceRatio;
    bool wantVertical = y > x * kDominanceRatio;
    if (!wantHorizontal && !wantVertical) {
        wantHorizontal = true;
        wantVertical = true;
    }

    bool attempted = false;
    bool anyApplied = false;

    if (wantHorizontal && g_horizontalScrollEnabled) {
        attempted = true;
        bool ok = ApplyMidiPixelAxis(event, "h", event.preciseDeltaX,
                                     g_midiHorizontalRemainder);
        anyApplied = anyApplied || ok;
    }
    if (wantVertical && g_verticalScrollEnabled) {
        attempted = true;
        bool ok = ApplyMidiPixelAxis(event, "v", event.preciseDeltaY,
                                     g_midiVerticalRemainder);
        anyApplied = anyApplied || ok;
    }

    // If neither corresponding custom axis is enabled, preserve native REAPER.
    if (!attempted) return false;
    // Fail open only when no custom axis could do anything at all. If one axis
    // succeeded in a diagonal gesture, suppress stock handling to avoid double
    // motion on that successful axis.
    return anyApplied;
}

bool ApplyMidiHorizontalPinch(const TrackpadEvent &event, double zoomAmount) {
    if (!event.isInMidiEditor || event.midiViewId == 0 || zoomAmount == 0.0)
        return false;

    ResolvePixelScrollApi();
    if (!g_midiEditorGetActive || !g_midiEditorOnCommand ||
        !g_jsGetScrollInfo || !g_jsSetScrollPos || event.viewWidthPixels <= 0.0) {
        return false;
    }

    HWND editor = g_midiEditorGetActive();
    if (!editor) return false;

    if (event.phase & (PhaseMayBegin | PhaseBegan)) g_midiPinchAccumulator = 0.0;
    g_midiPinchAccumulator += zoomAmount;

    // REAPER exposes MIDI zoom as actions rather than a continuous public API.
    // Accumulate small magnification deltas, then issue closely-spaced actions.
    // Existing PinchZoomProcessor still supplies velocity/momentum, so fast
    // gestures remain fluid without hammering commands on every tiny delta.
    constexpr double kZoomStep = 0.020;
    int steps = static_cast<int>(std::fabs(g_midiPinchAccumulator) / kZoomStep);
    if (steps == 0) return true;
    if (steps > 8) steps = 8;

    const bool zoomIn = g_midiPinchAccumulator > 0.0;
    const int commandId = zoomIn ? 1012 : 1011; // MIDI: horiz zoom in/out
    const double sign = zoomIn ? 1.0 : -1.0;

    double fraction = event.localX / event.viewWidthPixels;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    void *view = reinterpret_cast<void *>(event.midiViewId);

    bool didAnything = false;
    for (int i = 0; i < steps; ++i) {
        int pos = 0, page = 0, minPos = 0, maxPos = 0;
        if (!ReadMidiScrollInfo(view, "h", pos, page, minPos, maxPos))
            return didAnything;

        // Preserve the scroll-coordinate point currently under the mouse.
        double anchor = static_cast<double>(pos) + fraction * page;
        if (!g_midiEditorOnCommand(editor, commandId)) return didAnything;
        didAnything = true;

        int newPos = 0, newPage = 0, newMin = 0, newMax = 0;
        if (ReadMidiScrollInfo(view, "h", newPos, newPage, newMin, newMax)) {
            long long target = static_cast<long long>(std::llround(
                anchor - fraction * static_cast<double>(newPage)));
            int clamped = ClampScrollPosition(target, newPage, newMin, newMax);
            g_jsSetScrollPos(view, "h", clamped);
        }

        g_midiPinchAccumulator -= sign * kZoomStep;
    }
    return true;
}

bool ApplyMidiCommandVerticalZoom(const TrackpadEvent &event) {
    if (!event.isInMidiEditor || event.type != TrackpadEventType::Scroll ||
        !event.hasPreciseDeltas || event.modifiers != ModCommand) {
        return false;
    }

    double x = std::fabs(event.preciseDeltaX);
    double y = std::fabs(event.preciseDeltaY);
    if (y <= x * 3.0) return false; // leave Cmd-horizontal/diagonal native

    ResolvePixelScrollApi();
    if (!g_midiEditorGetActive || !g_midiEditorOnCommand) return false;
    HWND editor = g_midiEditorGetActive();
    if (!editor) return false;

    if (event.phase & (PhaseMayBegin | PhaseBegan))
        g_midiVerticalZoomAccumulator = 0.0;
    g_midiVerticalZoomAccumulator += event.preciseDeltaY;

    constexpr double kVerticalZoomPixelsPerStep = 10.0;
    int steps = static_cast<int>(std::fabs(g_midiVerticalZoomAccumulator) /
                                 kVerticalZoomPixelsPerStep);
    if (steps == 0) return true;
    if (steps > 8) steps = 8;

    bool zoomIn = g_midiVerticalZoomAccumulator > 0.0;
    int commandId = zoomIn ? 40111 : 40112; // MIDI: vertical zoom in/out
    double sign = zoomIn ? 1.0 : -1.0;
    for (int i = 0; i < steps; ++i) {
        if (!g_midiEditorOnCommand(editor, commandId)) return i > 0;
        g_midiVerticalZoomAccumulator -= sign * kVerticalZoomPixelsPerStep;
    }
    return true;
}

'''
if marker not in s:
    raise SystemExit('Arrange pinch marker not found')
s = s.replace(marker, helpers + marker, 1)

# MIDI must run before v3's generic "modified scroll -> native" guard.
needle = '''bool Apply(const TrackpadEvent &event, const NavigationCommand &command) {\n    Diagnostics::Trace("Navigation", "received NavigationCommand");\n\n    // Never swallow modified wheel/trackpad events. REAPER Action sections\n    // (Main or MIDI Editor) remain authoritative for modifier gestures.\n    if (event.type == TrackpadEventType::Scroll && event.modifiers != 0) {\n        return false;\n    }\n'''
repl = '''bool Apply(const TrackpadEvent &event, const NavigationCommand &command) {\n    Diagnostics::Trace("Navigation", "received NavigationCommand");\n\n    // MIDI is an explicitly-scoped branch: only events whose real receiver is\n    // inside midiview (ID 1001) can enter it. Arrange behavior below is left\n    // byte-for-byte in its existing path.\n    if (event.isInMidiEditor) {\n        if (event.type == TrackpadEventType::Scroll) {\n            if (ApplyMidiCommandVerticalZoom(event)) return true;\n            if (event.modifiers != 0) return false;\n            return ApplyMidiPan(event);\n        }\n        if (event.type == TrackpadEventType::Magnify) {\n            if (event.modifiers != 0 || !g_zoomEnabled) return false;\n            return ApplyMidiHorizontalPinch(event, command.zoomAmount);\n        }\n    }\n\n    // Outside MIDI, preserve the confirmed v3.2 modifier behavior.\n    if (event.type == TrackpadEventType::Scroll && event.modifiers != 0) {\n        return false;\n    }\n'''
if needle not in s:
    raise SystemExit('v3 modified-scroll Apply guard not found')
s = s.replace(needle, repl, 1)
p.write_text(s)

# ---------------------------------------------------------------------------
# PluginEntry: when a MIDI editor appears after startup, scan exactly that HWND
# once so its concrete SWELL classes are swizzled. No second dylib and no
# global NSEvent monitor interception.
# ---------------------------------------------------------------------------
p = root / 'src/PluginEntry.cpp'
s = p.read_text()

needle = '''bool g_hookCommandRegistered = false;\nbool g_toggleActionRegistered = false;\n'''
repl = needle + '''\nusing MIDIEditorGetActiveFn = HWND (*)();\nMIDIEditorGetActiveFn g_midiEditorGetActiveForSwizzle = nullptr;\nHWND g_lastSwizzledMidiEditor = nullptr;\n'''
if needle not in s:
    raise SystemExit('PluginEntry state marker not found')
s = s.replace(needle, repl, 1)

needle = '''void TimerTick() {\n    Diagnostics::Flush();\n'''
repl = '''void TimerTick() {\n    Diagnostics::Flush();\n\n    // MIDI editors may be created after the extension loads. Scan only when\n    // the active MIDI editor HWND changes; this is event-driven in practice\n    // and avoids repeatedly walking the Cocoa hierarchy.\n    if (g_midiEditorGetActiveForSwizzle) {\n        HWND activeMidi = g_midiEditorGetActiveForSwizzle();\n        if (activeMidi && activeMidi != g_lastSwizzledMidiEditor) {\n            ViewSwizzleInterceptor::RefreshWindow((void *)activeMidi);\n            g_lastSwizzledMidiEditor = activeMidi;\n        } else if (!activeMidi) {\n            g_lastSwizzledMidiEditor = nullptr;\n        }\n    }\n'''
if needle not in s:
    raise SystemExit('TimerTick marker not found')
s = s.replace(needle, repl, 1)

needle = '''    ReaperNavigation::SetApiResolver(rec->GetFunc);\n'''
repl = needle + '''    g_midiEditorGetActiveForSwizzle = reinterpret_cast<MIDIEditorGetActiveFn>(\n        rec->GetFunc("MIDIEditor_GetActive"));\n'''
if needle not in s:
    raise SystemExit('SetApiResolver marker not found in PluginEntry')
s = s.replace(needle, repl, 1)

# Clear cached pointer during unload before code is unmapped.
needle = '''        ViewSwizzleInterceptor::Remove();\n        TrackpadInterceptor::Remove();\n'''
repl = '''        ViewSwizzleInterceptor::Remove();\n        TrackpadInterceptor::Remove();\n        g_lastSwizzledMidiEditor = nullptr;\n        g_midiEditorGetActiveForSwizzle = nullptr;\n'''
if needle not in s:
    raise SystemExit('unload marker not found')
s = s.replace(needle, repl, 1)
p.write_text(s)

print('v3.3 integrated MIDI patch applied')
