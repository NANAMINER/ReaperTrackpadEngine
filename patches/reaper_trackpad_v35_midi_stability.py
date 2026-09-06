from pathlib import Path

root = Path('source')

# ---------------------------------------------------------------------------
# v3.5 MIDI stability pass.
# Goals:
#   1) never let native MIDI scroll/magnify leak through while our matching
#      custom gesture is enabled (prevents custom+native double motion),
#   2) classify MIDI events robustly even when SWELL delivers magnify to an
#      ancestor/root view rather than directly to midiview,
#   3) make vertical pan independent of delayed scrollbar readback,
#   4) remove MIDI pinch momentum and make mouse-anchor correction run from
#      TimerTick so the final zoom step is corrected too,
#   5) lower pinch sensitivity and avoid fallback double-dispatch.
# Arrange v3.2 path remains untouched.
# ---------------------------------------------------------------------------

# --- View targeting ---------------------------------------------------------
p = root / 'src/ViewSwizzleInterceptor.mm'
s = p.read_text()
old = '''    NSView *arrangeAncestor = FindArrangeViewInHierarchy(view);\n    NSView *arrangeView = arrangeAncestor ?: FindViewWithTag(gMainView, kArrangeViewTag);\n    NSView *midiNotesView = FindMidiNotesViewInHierarchy(view);\n    te.arrangeViewId = (uintptr_t)(__bridge void *)arrangeView;\n    te.midiViewId = (uintptr_t)(__bridge void *)midiNotesView;\n    te.isInMidiEditor = midiNotesView != nil;\n    te.isInArrangeView = arrangeAncestor != nil;\n'''
new = '''    NSView *arrangeAncestor = FindArrangeViewInHierarchy(view);\n    NSView *arrangeView = arrangeAncestor ?: FindViewWithTag(gMainView, kArrangeViewTag);\n    NSView *midiNotesView = FindMidiNotesViewInHierarchy(view);\n\n    // SWELL does not guarantee that magnifyWithEvent: is delivered to the\n    // exact same child that receives scrollWheel:. If the receiver is an\n    // ancestor/root view, locate the real midiview (tag 1001) in this window\n    // and accept it only when the event is physically inside that grid. This\n    // prevents alternating custom/native handling during one pinch.\n    if (!midiNotesView && view.window && view.window.contentView) {\n        NSView *candidate = FindViewWithTag(view.window.contentView, kMidiNotesViewTag);\n        if (candidate) {\n            NSRect midiRect = [candidate convertRect:candidate.bounds toView:nil];\n            if (NSPointInRect(event.locationInWindow, midiRect)) {\n                midiNotesView = candidate;\n            }\n        }\n    }\n\n    te.arrangeViewId = (uintptr_t)(__bridge void *)arrangeView;\n    te.midiViewId = (uintptr_t)(__bridge void *)midiNotesView;\n    te.isInMidiEditor = midiNotesView != nil;\n    te.isInArrangeView = arrangeAncestor != nil;\n'''
if old not in s:
    raise SystemExit('v3.5 target context block not found')
s = s.replace(old, new, 1)
p.write_text(s)

# --- Navigation -------------------------------------------------------------
p = root / 'src/ReaperNavigation.cpp'
s = p.read_text()

needle = '''double g_midiVerticalRemainder = 0.0;\ndouble g_midiPinchAccumulator = 0.0;\ndouble g_midiVerticalZoomAccumulator = 0.0;\nuintptr_t g_midiGestureViewId = 0;\n'''
repl = '''double g_midiVerticalRemainder = 0.0;\ndouble g_midiPinchAccumulator = 0.0;\ndouble g_midiVerticalZoomAccumulator = 0.0;\nuintptr_t g_midiGestureViewId = 0;\n\n// Vertical scrollbar updates in REAPER can publish their new position one UI\n// turn later. Keep our own absolute target during a gesture so high-rate\n// trackpad packets do not repeatedly read a stale position and then jump.\ndouble g_midiVerticalTarget = 0.0;\nbool g_midiVerticalTargetValid = false;\n'''
if needle not in s:
    raise SystemExit('v3.5 MIDI state marker not found')
s = s.replace(needle, repl, 1)

old = r'''bool ApplyMidiPixelAxis(const TrackpadEvent &event, const char *axis,
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
new = r'''bool ApplyMidiPixelAxis(const TrackpadEvent &event, const char *axis,
                        double delta, double &remainder) {
    ResolvePixelScrollApi();
    if (!g_jsGetScrollInfo || !g_jsSetScrollPos || event.midiViewId == 0)
        return false;

    void *view = reinterpret_cast<void *>(event.midiViewId);
    int pos = 0, page = 0, minPos = 0, maxPos = 0;
    if (!ReadMidiScrollInfo(view, axis, pos, page, minPos, maxPos)) return false;

    const bool vertical = axis && axis[0] == 'v';
    double logicalDelta = delta;
    if (vertical && event.viewHeightPixels > 1.0 && page > 0) {
        // SCROLLINFO::nPage is expressed in the scrollbar's own logical units.
        // Map Apple precise points into that coordinate space.
        double unitsPerPoint = static_cast<double>(page) / event.viewHeightPixels;
        if (unitsPerPoint < 0.05) unitsPerPoint = 0.05;
        if (unitsPerPoint > 512.0) unitsPerPoint = 512.0;
        logicalDelta *= unitsPerPoint;
    }

    if (vertical) {
        // Do NOT rebase every packet on nPos. MIDI vertical scroll state can
        // lag the setter by a UI turn; rebasing on that stale value causes
        // visible stick/jump motion. Track a continuous absolute target for
        // the entire gesture instead.
        if (!g_midiVerticalTargetValid) {
            g_midiVerticalTarget = static_cast<double>(pos);
            g_midiVerticalTargetValid = true;
        }
        g_midiVerticalTarget -= logicalDelta;

        double effectiveMax = static_cast<double>(maxPos) -
                              static_cast<double>(page > 0 ? page - 1 : 0);
        if (effectiveMax < minPos) effectiveMax = minPos;
        if (g_midiVerticalTarget < minPos) g_midiVerticalTarget = minPos;
        if (g_midiVerticalTarget > effectiveMax) g_midiVerticalTarget = effectiveMax;

        int target = static_cast<int>(std::llround(g_midiVerticalTarget));
        if (target != pos) {
            // The API's bool is not used as a pass-through decision. Once we
            // own the MIDI gesture, falling through to REAPER's stock wheel
            // handler would layer native zoom/scroll over this motion.
            g_jsSetScrollPos(view, axis, target);
        }
        return true;
    }

    // Horizontal behavior was already confirmed good by the user. Preserve
    // its fractional remainder model, but still consume after dispatch so a
    // transient false return cannot invoke the native MIDI wheel handler too.
    remainder += logicalDelta;
    int units = static_cast<int>(remainder);
    if (units == 0) return true;
    remainder -= static_cast<double>(units);

    int target = ClampScrollPosition(static_cast<long long>(pos) - units,
                                     page, minPos, maxPos);
    if (target != pos) g_jsSetScrollPos(view, axis, target);
    return true;
}
'''
if old not in s:
    raise SystemExit('v3.5 ApplyMidiPixelAxis block not found')
s = s.replace(old, new, 1)

old = r'''void ResetMidiPanStateIfNeeded(const TrackpadEvent &event) {
    if (g_midiGestureViewId != event.midiViewId ||
        (event.phase & (PhaseMayBegin | PhaseBegan))) {
        g_midiGestureViewId = event.midiViewId;
        g_midiHorizontalRemainder = 0.0;
        g_midiVerticalRemainder = 0.0;
    }
}
'''
new = r'''void ResetMidiPanStateIfNeeded(const TrackpadEvent &event) {
    if (g_midiGestureViewId != event.midiViewId ||
        (event.phase & (PhaseMayBegin | PhaseBegan))) {
        g_midiGestureViewId = event.midiViewId;
        g_midiHorizontalRemainder = 0.0;
        g_midiVerticalRemainder = 0.0;
        g_midiVerticalTargetValid = false;
    }
}
'''
if old not in s:
    raise SystemExit('v3.5 ResetMidiPanStateIfNeeded block not found')
s = s.replace(old, new, 1)

# Once a matching custom MIDI pan is enabled, it owns the whole gesture.
# Never decide pass-through from an API setter return value.
old = '''    // If neither corresponding custom axis is enabled, preserve native REAPER.\n    if (!attempted) return false;\n    // Fail open only when no custom axis could do anything at all. If one axis\n    // succeeded in a diagonal gesture, suppress stock handling to avoid double\n    // motion on that successful axis.\n    return anyApplied;\n}\n'''
new = '''    // If neither corresponding custom axis is enabled, preserve native REAPER.\n    if (!attempted) return false;\n\n    // A matching custom MIDI pan owns the event even if a setter reports\n    // false or the viewport is at a boundary. Passing such packets through\n    // alternates our pixel path with REAPER's stock MIDI wheel behavior.\n    (void)anyApplied;\n    return true;\n}\n'''
if old not in s:
    raise SystemExit('v3.5 ApplyMidiPan tail not found')
s = s.replace(old, new, 1)

old = r'''bool DispatchMidiRelativeAction(int commandId, bool positive, HWND editor) {
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
'''
new = r'''bool DispatchMidiRelativeAction(int commandId, bool positive, HWND editor) {
    KbdSectionInfo *section = GetMidiActionSection();
    if (!section || !section->onAction || !MidiSectionHasCommand(section, commandId))
        return false;

    // Relative mode 1 is REAPER's signed relative CC mode. The section's
    // onAction return value is a handler result, not a reliable "view changed"
    // acknowledgement. Calling a fixed fallback when it returns false can
    // therefore execute TWO zoom operations for one trackpad packet. If the
    // command exists in the section, dispatch it exactly once and consider
    // that path authoritative.
    int val = positive ? 1 : 127;
    section->onAction(commandId, val, -1, 1, editor);
    return true;
}
'''
if old not in s:
    raise SystemExit('v3.5 DispatchMidiRelativeAction block not found')
s = s.replace(old, new, 1)

start = s.index('bool ApplyMidiHorizontalPinch(const TrackpadEvent &event, double zoomAmount) {')
end = s.index('\nbool ApplyMidiCommandVerticalZoom(const TrackpadEvent &event) {', start)
new_block = r'''bool ApplyMidiHorizontalPinch(const TrackpadEvent &event, double zoomAmount) {
    if (!event.isInMidiEditor || event.midiViewId == 0) return false;

    // IMPORTANT: zero-delta Ended/Cancelled magnify packets still belong to
    // the custom MIDI pinch. v3.4 returned false for them, which called the
    // original REAPER magnify handler at gesture end and could snap the view
    // back toward REAPER's own cursor/play-start anchor.
    if (!g_zoomEnabled) return false;
    if (zoomAmount == 0.0) return true;

    ResolvePixelScrollApi();
    if (!g_midiEditorGetActive || event.viewWidthPixels <= 0.0) {
        // Own the gesture even if the custom API is temporarily unavailable;
        // mixing native magnify into an otherwise-custom pinch is worse than
        // dropping one packet.
        return true;
    }
    HWND editor = g_midiEditorGetActive();
    if (!editor) return true;

    if (event.phase & (PhaseMayBegin | PhaseBegan)) {
        g_midiPinchAccumulator = 0.0;
        g_midiPendingAnchor.active = false;
    }

    // Do not issue a second zoom action until TimerTick has corrected the
    // previous step's mouse anchor using REAPER's *new* scrollbar metrics.
    // This also rate-limits action bursts to UI cadence.
    g_midiPinchAccumulator += zoomAmount;
    constexpr double kMaxQueuedPinch = 0.12;
    if (g_midiPinchAccumulator > kMaxQueuedPinch)
        g_midiPinchAccumulator = kMaxQueuedPinch;
    if (g_midiPinchAccumulator < -kMaxQueuedPinch)
        g_midiPinchAccumulator = -kMaxQueuedPinch;
    if (g_midiPendingAnchor.active) return true;

    // v3.4 used 0.010 and felt much too sensitive. 0.035 requires roughly
    // 3.5x more physical magnification per REAPER relative zoom quantum.
    constexpr double kZoomQuantum = 0.035;
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

    // 990 = relative horizontal zoom. If this running REAPER build exposes
    // it in section 32060, dispatch exactly once. Only if the command itself
    // is absent do we use the fixed keyboard fallback.
    bool dispatched = DispatchMidiRelativeAction(990, zoomIn, editor);
    if (!dispatched && g_midiEditorOnCommand) {
        g_midiEditorOnCommand(editor, zoomIn ? 1012 : 1011);
        dispatched = true;
    }

    // Regardless of host acknowledgement, never pass the same magnify packet
    // to native REAPER after attempting our MIDI zoom.
    if (!dispatched) return true;

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

start = s.index('bool ApplyMidiCommandVerticalZoom(const TrackpadEvent &event) {')
end = s.index('\n\n// Pinch-zoom, anchored on REAPER', start)
new_vz = r'''bool ApplyMidiCommandVerticalZoom(const TrackpadEvent &event) {
    if (!event.isInMidiEditor || event.type != TrackpadEventType::Scroll ||
        !event.hasPreciseDeltas || event.modifiers != ModCommand) {
        return false;
    }

    double x = std::fabs(event.preciseDeltaX);
    double y = std::fabs(event.preciseDeltaY);
    if (y <= x * 3.0) return false;

    // From this point the custom Cmd+vertical gesture owns the event. Never
    // leak a threshold/boundary/API-failure packet to stock REAPER, which is
    // what produces alternating/doubled zoom steps.
    ResolvePixelScrollApi();
    if (!g_midiEditorGetActive) return true;
    HWND editor = g_midiEditorGetActive();
    if (!editor) return true;

    if (event.phase & (PhaseMayBegin | PhaseBegan))
        g_midiVerticalZoomAccumulator = 0.0;
    g_midiVerticalZoomAccumulator += event.preciseDeltaY;

    // Smallest relative REAPER quantum is already discrete. Space those
    // quanta farther apart than v3.4 so one physical swipe cannot burst many
    // UI zoom steps. No synthetic momentum is added for MIDI.
    constexpr double kVerticalZoomQuantum = 8.0;
    if (std::fabs(g_midiVerticalZoomAccumulator) < kVerticalZoomQuantum)
        return true;

    bool zoomIn = g_midiVerticalZoomAccumulator > 0.0;
    double sign = zoomIn ? 1.0 : -1.0;

    bool dispatched = DispatchMidiRelativeAction(991, zoomIn, editor);
    if (!dispatched && g_midiEditorOnCommand) {
        g_midiEditorOnCommand(editor, zoomIn ? 40111 : 40112);
        dispatched = true;
    }

    if (dispatched)
        g_midiVerticalZoomAccumulator -= sign * kVerticalZoomQuantum;
    return true;
}
'''
s = s[:start] + new_vz + s[end:]

# Public timer hook for the deferred mouse-anchor correction. This solves the
# final-step bug: v3.4 only corrected at the next magnify event, so the last
# zoom operation of a gesture could remain anchored at REAPER's native cursor.
marker = 'void SetApiResolver(void *(*getFunc)(const char *)) {\n'
if marker not in s:
    raise SystemExit('v3.5 SetApiResolver marker not found')
insert = '''void TickMidiDeferredUi() {\n    ApplyPendingMidiAnchor();\n}\n\n'''
s = s.replace(marker, insert + marker, 1)
p.write_text(s)

p = root / 'src/ReaperNavigation.h'
s = p.read_text()
needle = '''void SetApiResolver(void *(*getFunc)(const char *));\n'''
repl = needle + '''\n// Called from REAPER's UI timer. Applies the previous MIDI zoom step's mouse\n// anchor after REAPER has published fresh scrollbar metrics.\nvoid TickMidiDeferredUi();\n'''
if needle not in s:
    raise SystemExit('v3.5 ReaperNavigation.h resolver marker not found')
s = s.replace(needle, repl, 1)
p.write_text(s)

# --- Disable synthetic pinch momentum specifically in MIDI ------------------
p = root / 'src/PinchZoomProcessor.cpp'
s = p.read_text()
old = '''        g_momentumActive = ended && g_hasPrevEventTimestamp &&\n                           std::fabs(g_velocity) > g_stopThreshold;\n        g_lastTickTime = std::chrono::steady_clock::now();\n'''
new = '''        // MIDI zoom actions are discrete host operations; synthesized pinch\n        // momentum makes them overshoot and can continue after the user's\n        // fingers have stopped. Arrange keeps its existing momentum behavior.\n        g_momentumActive = !event.isInMidiEditor && ended && g_hasPrevEventTimestamp &&\n                           std::fabs(g_velocity) > g_stopThreshold;\n        g_lastTickTime = std::chrono::steady_clock::now();\n'''
if old not in s:
    raise SystemExit('v3.5 pinch momentum marker not found')
s = s.replace(old, new, 1)
p.write_text(s)

# --- Timer: always flush deferred MIDI anchor -------------------------------
p = root / 'src/PluginEntry.cpp'
s = p.read_text()
marker = '''    // Advances any active synthesized pinch-zoom momentum (see\n'''
if marker not in s:
    raise SystemExit('v3.5 TimerTick momentum comment marker not found')
s = s.replace(marker, '''    // Finish any MIDI zoom anchor correction after one REAPER UI turn.\n    ReaperNavigation::TickMidiDeferredUi();\n\n''' + marker, 1)
p.write_text(s)

print('v3.5 MIDI stability patch applied')
