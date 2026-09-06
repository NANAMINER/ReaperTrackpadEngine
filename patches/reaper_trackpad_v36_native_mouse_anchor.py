from pathlib import Path

root = Path('source')
p = root / 'src/ReaperNavigation.cpp'
s = p.read_text()

# v3.6: stop fighting REAPER's own MIDI zoom anchor after the fact.
# REAPER's documented/configured horizontal zoom center is the integer config
# variable `zoommode`: 0 edit/play cursor, 1 edit cursor, 2 view center,
# 3 mouse cursor. Previous builds invoked the host MIDI zoom while zoommode was
# commonly 0 and then tried to repair the scrollbar position afterward. REAPER
# can finish its own MIDI viewport update later, so the two anchors visibly
# fought each other. Use exactly one host zoom path and make its own anchor the
# mouse cursor instead.

needle = '''using MIDIEditorGetActiveFn = HWND (*)();\nusing MIDIEditorOnCommandFn = bool (*)(HWND, int);\nusing SectionFromUniqueIDFn = KbdSectionInfo *(*)(int);\n\nReaperGetFunc g_reaperGetFunc = nullptr;\nJSGetScrollInfoFn g_jsGetScrollInfo = nullptr;\nJSSetScrollPosFn g_jsSetScrollPos = nullptr;\nMIDIEditorGetActiveFn g_midiEditorGetActive = nullptr;\nMIDIEditorOnCommandFn g_midiEditorOnCommand = nullptr;\nSectionFromUniqueIDFn g_sectionFromUniqueID = nullptr;\n'''
repl = '''using MIDIEditorGetActiveFn = HWND (*)();\nusing MIDIEditorOnCommandFn = bool (*)(HWND, int);\nusing SectionFromUniqueIDFn = KbdSectionInfo *(*)(int);\nusing GetConfigVarFn = void *(*)(const char *, int *);\n\nReaperGetFunc g_reaperGetFunc = nullptr;\nJSGetScrollInfoFn g_jsGetScrollInfo = nullptr;\nJSSetScrollPosFn g_jsSetScrollPos = nullptr;\nMIDIEditorGetActiveFn g_midiEditorGetActive = nullptr;\nMIDIEditorOnCommandFn g_midiEditorOnCommand = nullptr;\nSectionFromUniqueIDFn g_sectionFromUniqueID = nullptr;\nGetConfigVarFn g_getConfigVar = nullptr;\nint *g_zoomModeConfig = nullptr;\nint g_savedZoomMode = 0;\nbool g_zoomModeOverridden = false;\n'''
if needle not in s:
    raise SystemExit('v3.6 resolver declaration marker not found')
s = s.replace(needle, repl, 1)

needle = '''    if (!g_sectionFromUniqueID) {\n        g_sectionFromUniqueID = reinterpret_cast<SectionFromUniqueIDFn>(\n            g_reaperGetFunc("SectionFromUniqueID"));\n    }\n}\n'''
repl = '''    if (!g_sectionFromUniqueID) {\n        g_sectionFromUniqueID = reinterpret_cast<SectionFromUniqueIDFn>(\n            g_reaperGetFunc("SectionFromUniqueID"));\n    }\n    if (!g_getConfigVar) {\n        g_getConfigVar = reinterpret_cast<GetConfigVarFn>(\n            g_reaperGetFunc("get_config_var"));\n    }\n    if (!g_zoomModeConfig && g_getConfigVar) {\n        int size = 0;\n        void *ptr = g_getConfigVar("zoommode", &size);\n        if (ptr && size == static_cast<int>(sizeof(int)))\n            g_zoomModeConfig = static_cast<int *>(ptr);\n    }\n}\n'''
if needle not in s:
    raise SystemExit('v3.6 resolver end marker not found')
s = s.replace(needle, repl, 1)

marker = '''KbdSectionInfo *GetMidiActionSection() {\n'''
helpers = r'''bool EnsureMidiMouseZoomCenter() {
    ResolvePixelScrollApi();
    if (!g_zoomModeConfig) return false;

    if (!g_zoomModeOverridden) {
        g_savedZoomMode = *g_zoomModeConfig;
        g_zoomModeOverridden = true;
    }

    // REAPER zoommode: 0 edit/play cursor, 1 edit cursor,
    // 2 center of view, 3 mouse cursor.
    if (*g_zoomModeConfig != 3) *g_zoomModeConfig = 3;
    return true;
}

void RestoreMidiMouseZoomCenter() {
    if (g_zoomModeOverridden && g_zoomModeConfig) {
        *g_zoomModeConfig = g_savedZoomMode;
    }
    g_zoomModeOverridden = false;
}

'''
if marker not in s:
    raise SystemExit('v3.6 GetMidiActionSection marker not found')
s = s.replace(marker, helpers + marker, 1)

start = s.index('bool ApplyMidiHorizontalPinch(const TrackpadEvent &event, double zoomAmount) {')
end = s.index('\nbool ApplyMidiCommandVerticalZoom(const TrackpadEvent &event) {', start)
new_block = r'''bool ApplyMidiHorizontalPinch(const TrackpadEvent &event, double zoomAmount) {
    if (!event.isInMidiEditor || event.midiViewId == 0) return false;
    if (!g_zoomEnabled) return false;

    // Once classified as MIDI pinch, own every packet (including zero-delta
    // Ended/Cancelled packets) so Cocoa/REAPER never sees the same gesture as
    // a second zoom path.
    if (zoomAmount == 0.0) return true;

    ResolvePixelScrollApi();
    if (!g_midiEditorGetActive) return true;
    HWND editor = g_midiEditorGetActive();
    if (!editor) return true;

    if (event.phase & (PhaseMayBegin | PhaseBegan)) {
        g_midiPinchAccumulator = 0.0;
        // v3.5's deferred scrollbar correction is intentionally retired.
        // There must be no second actor moving the MIDI viewport after zoom.
        g_midiPendingAnchor.active = false;
    }

    // Make REAPER's OWN MIDI zoom center the pointer. This is the key v3.6
    // architecture change: one zoom operation, one anchor, no later repair.
    EnsureMidiMouseZoomCenter();

    g_midiPinchAccumulator += zoomAmount;
    constexpr double kMaxQueuedPinch = 0.18;
    if (g_midiPinchAccumulator > kMaxQueuedPinch)
        g_midiPinchAccumulator = kMaxQueuedPinch;
    if (g_midiPinchAccumulator < -kMaxQueuedPinch)
        g_midiPinchAccumulator = -kMaxQueuedPinch;

    // Deliberately less sensitive than v3.5 (0.035). The host action already
    // has a discrete minimum zoom quantum, so sending it less often is the only
    // honest way to make a physical pinch gentler without inventing a second
    // interpolated viewport.
    constexpr double kZoomQuantum = 0.060;
    if (std::fabs(g_midiPinchAccumulator) < kZoomQuantum) return true;

    const bool zoomIn = g_midiPinchAccumulator > 0.0;
    const double sign = zoomIn ? 1.0 : -1.0;

    // 990 = MIDI Editor: View: Zoom horizontally
    // (MIDI CC relative/mousewheel). Dispatch exactly once. No scrollbar
    // correction before or after it, and no second fixed command unless the
    // relative action itself is absent from this REAPER build.
    bool dispatched = DispatchMidiRelativeAction(990, zoomIn, editor);
    if (!dispatched && g_midiEditorOnCommand) {
        g_midiEditorOnCommand(editor, zoomIn ? 1012 : 1011);
        dispatched = true;
    }

    if (dispatched)
        g_midiPinchAccumulator -= sign * kZoomQuantum;

    // Even on API failure, consume this packet rather than layering REAPER's
    // original magnify handler on top of the custom gesture.
    return true;
}
'''
s = s[:start] + new_block + s[end:]

old = '''void TickMidiDeferredUi() {\n    ApplyPendingMidiAnchor();\n}\n'''
new = '''void TickMidiDeferredUi() {\n    // v3.6: intentionally empty. MIDI pinch uses REAPER zoommode=mouse and\n    // never performs a second scrollbar correction.\n}\n'''
if old not in s:
    raise SystemExit('v3.6 TickMidiDeferredUi block not found')
s = s.replace(old, new, 1)

marker = '''void SetApiResolver(void *(*getFunc)(const char *)) {\n'''
insert = '''void RestoreHostPreferences() {\n    RestoreMidiMouseZoomCenter();\n}\n\n'''
if marker not in s:
    raise SystemExit('v3.6 SetApiResolver marker missing')
s = s.replace(marker, insert + marker, 1)
p.write_text(s)

p = root / 'src/ReaperNavigation.h'
s = p.read_text()
needle = '''void TickMidiDeferredUi();\n'''
repl = needle + '''\n// Restores any REAPER preference temporarily overridden by the gesture\n// engine (currently horizontal zoom center) before the extension unloads.\nvoid RestoreHostPreferences();\n'''
if needle not in s:
    raise SystemExit('v3.6 header TickMidiDeferredUi marker not found')
s = s.replace(needle, repl, 1)
p.write_text(s)

p = root / 'src/PluginEntry.cpp'
s = p.read_text()
# Previous MIDI patches may insert refresh/cleanup code between interceptor
# removal and UnregisterAll, so anchor on the unique unload call itself.
marker = '''        UnregisterAll();\n'''
if marker not in s:
    raise SystemExit('v3.6 UnregisterAll unload marker not found')
s = s.replace(marker,
    '''        ReaperNavigation::RestoreHostPreferences();\n        UnregisterAll();\n''', 1)
p.write_text(s)

print('v3.6 native mouse-anchor MIDI zoom applied')
