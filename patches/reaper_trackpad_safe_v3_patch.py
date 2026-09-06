from pathlib import Path

root = Path('source')

# Reproduce the last known-good Arrange behavior, but resolve GetTrackWnd
# dynamically because current REAPER SDK headers no longer declare it.
p = root / 'src/VerticalScrollProcessor.cpp'
p.write_text(r'''#include "VerticalScrollProcessor.h"

namespace VerticalScrollProcessor {
namespace {

double g_sensitivity = 1.0;
double g_remainderPixels = 0.0;

} // namespace

double Process(const TrackpadEvent &event) {
    if (event.type != TrackpadEventType::Scroll) return 0.0;
    if (!event.hasPreciseDeltas) return 0.0;

    g_remainderPixels += event.preciseDeltaY * g_sensitivity;
    int pixels = static_cast<int>(g_remainderPixels);
    if (pixels == 0) return 0.0;
    g_remainderPixels -= static_cast<double>(pixels);
    return static_cast<double>(pixels);
}

void Reset() {
    g_remainderPixels = 0.0;
}

void SetSensitivity(double sensitivity) { g_sensitivity = sensitivity; }
double GetSensitivity() { return g_sensitivity; }

} // namespace VerticalScrollProcessor
''')

p = root / 'src/VerticalScrollProcessor.h'
p.write_text(r'''#pragma once

#include "TrackpadEvent.h"

namespace VerticalScrollProcessor {

double Process(const TrackpadEvent &event);
void Reset();
void SetSensitivity(double sensitivity);
double GetSensitivity();

} // namespace VerticalScrollProcessor
''')

p = root / 'src/ReaperNavigation.cpp'
s = p.read_text()

marker = 'bool g_zoomEnabled = false;\n'
insert = r'''

using ReaperGetFunc = void *(*)(const char *);
using CFGetScrollInfoFn = int (*)(HWND, int, SCROLLINFO *);
using CFSetTcpScrollFn = void (*)(MediaTrack *, int);
using GetTrackWndFn = HWND (*)();

ReaperGetFunc g_reaperGetFunc = nullptr;
CFGetScrollInfoFn g_cfGetScrollInfo = nullptr;
CFSetTcpScrollFn g_cfSetTcpScroll = nullptr;
GetTrackWndFn g_getTrackWnd = nullptr;

void ResolveSwsScrollApi() {
    if (!g_reaperGetFunc) return;
    if (!g_cfGetScrollInfo) {
        g_cfGetScrollInfo = reinterpret_cast<CFGetScrollInfoFn>(
            g_reaperGetFunc("CF_GetScrollInfo"));
    }
    if (!g_cfSetTcpScroll) {
        g_cfSetTcpScroll = reinterpret_cast<CFSetTcpScrollFn>(
            g_reaperGetFunc("CF_SetTcpScroll"));
    }
    if (!g_getTrackWnd) {
        g_getTrackWnd = reinterpret_cast<GetTrackWndFn>(
            g_reaperGetFunc("GetTrackWnd"));
    }
}
'''
if marker not in s:
    raise SystemExit('zoom marker not found')
s = s.replace(marker, marker + insert, 1)

old = r'''bool ApplyVerticalScroll(const TrackpadEvent &event, double stepAmount) {
    if (stepAmount == 0.0) return false;
    if (!event.isInArrangeView && !event.isInTrackControlPanel) return false;

    int steps = static_cast<int>(stepAmount);
    if (steps == 0) return false;

    CSurf_OnScroll(0, -steps);
    return true;
}
'''
if old not in s:
    old = r'''bool ApplyVerticalScroll(const TrackpadEvent &event, double stepAmount) {
    if (stepAmount == 0.0) return false;
    if (!event.isInArrangeView && !event.isInTrackControlPanel) return false;

    // VerticalScrollProcessor guarantees integer-valued output. Live testing
    // established that the direct sign feels reversed, so translate between
    // NSEvent's preciseDeltaY convention and CSurf_OnScroll by negating it.
    int steps = static_cast<int>(stepAmount);
    if (steps == 0) return false;

    CSurf_OnScroll(0, -steps);
    return true;
}
'''
new = r'''bool ApplyVerticalScroll(const TrackpadEvent &event, double pixelAmount) {
    if (pixelAmount == 0.0) return false;
    if (!event.isInArrangeView && !event.isInTrackControlPanel) return false;

    int pixels = static_cast<int>(pixelAmount);
    if (pixels == 0) return false;

    ResolveSwsScrollApi();
    if (!g_cfGetScrollInfo || !g_cfSetTcpScroll || !g_getTrackWnd) return false;

    HWND trackWnd = g_getTrackWnd();
    if (!trackWnd) return false;

    SCROLLINFO si = { sizeof(SCROLLINFO), };
    si.fMask = SIF_ALL;
    if (!g_cfGetScrollInfo(trackWnd, SB_VERT, &si)) return false;

    long long target = static_cast<long long>(si.nPos) - pixels;
    long long minPos = si.nMin;
    long long maxPos = static_cast<long long>(si.nMax) -
                       static_cast<long long>(si.nPage) + 1;
    if (maxPos < minPos) maxPos = minPos;
    if (target < minPos) target = minPos;
    if (target > maxPos) target = maxPos;

    if (static_cast<int>(target) != si.nPos) {
        g_cfSetTcpScroll(nullptr, static_cast<int>(target));
    }
    return true;
}
'''
if old not in s:
    raise SystemExit('ApplyVerticalScroll block not found')
s = s.replace(old, new, 1)

old = r'''    // Anchor on REAPER's edit cursor (the vertical timeline cursor), not the
    // macOS mouse pointer. Keeping its current fractional screen position
    // fixed avoids a visual jump even when it is not centered in the view.
    double anchorTime = GetCursorPositionEx(nullptr);
    double fraction = (anchorTime - start) / duration;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;

    // If the edit cursor is outside the visible span, anchor to the nearest
    // visible edge rather than extrapolating the Arrange view off-screen.
    anchorTime = start + fraction * duration;
'''
new = r'''    // Anchor pinch under the mouse/gesture position in Arrange.
    if (event.viewWidthPixels <= 0.0) return false;
    double fraction = event.localX / event.viewWidthPixels;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    double anchorTime = start + fraction * duration;
'''
if old not in s:
    raise SystemExit('pinch anchor block not found')
s = s.replace(old, new, 1)

old = 'bool Apply(const TrackpadEvent &event, const NavigationCommand &command) {\n    Diagnostics::Trace("Navigation", "received NavigationCommand");\n'
new = 'bool Apply(const TrackpadEvent &event, const NavigationCommand &command) {\n    Diagnostics::Trace("Navigation", "received NavigationCommand");\n\n    // Never swallow modified wheel/trackpad events. REAPER Action sections\n    // (Main or MIDI Editor) remain authoritative for modifier gestures.\n    if (event.type == TrackpadEventType::Scroll && event.modifiers != 0) {\n        return false;\n    }\n'
if old not in s:
    raise SystemExit('Apply marker not found')
s = s.replace(old, new, 1)

marker = 'void SetHorizontalScrollEnabled(bool enabled) { g_horizontalScrollEnabled = enabled; }\n'
repl = 'void SetApiResolver(void *(*getFunc)(const char *)) {\n    g_reaperGetFunc = getFunc;\n    ResolveSwsScrollApi();\n}\n\n' + marker
if marker not in s:
    raise SystemExit('setter marker not found')
s = s.replace(marker, repl, 1)
p.write_text(s)

p = root / 'src/ReaperNavigation.h'
s = p.read_text()
marker = 'namespace ReaperNavigation {\n'
addition = '\nvoid SetApiResolver(void *(*getFunc)(const char *));\n'
if marker not in s:
    raise SystemExit('header namespace marker not found')
s = s.replace(marker, marker + addition, 1)
p.write_text(s)

p = root / 'src/PluginEntry.cpp'
s = p.read_text()
needle = r'''    if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) {
        // One of the functions listed via REAPERAPI_WANT_* above failed to
        // resolve -- refuse to load rather than run with null function
        // pointers.
        return 0;
    }
'''
replacement = needle + r'''
    ReaperNavigation::SetApiResolver(rec->GetFunc);
'''
if needle not in s:
    raise SystemExit('REAPERAPI_LoadAPI block not found')
s = s.replace(needle, replacement, 1)

# Silent startup only. Keep manual diagnostics available.
banner = s.find('[TrackpadEngine] loaded')
if banner < 0:
    raise SystemExit('startup banner marker missing')
console_call = s.find('ConsoleLog(buf);', banner)
if console_call < 0:
    raise SystemExit('startup ConsoleLog call missing')
s = s[:console_call] + '// startup intentionally silent' + s[console_call + len('ConsoleLog(buf);'):]
p.write_text(s)

print('safe v3 patch applied')
