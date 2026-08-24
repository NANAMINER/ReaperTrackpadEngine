#include "Diagnostics.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <pthread.h>

namespace Diagnostics {
namespace {

// Fixed-capacity static ring: no heap allocation and no I/O on the capture
// path. View-hierarchy snapshots made each record larger than the original
// Phase-1 layout, but the allocation remains bounded and process-lifetime.
constexpr size_t kCapacity = 16384;

DiagnosticEvent g_buffer[kCapacity];
size_t g_writePos = 0;
size_t g_readPos = 0;
size_t g_size = 0;       // valid, unread entries currently in the ring
uint64_t g_dropped = 0;  // entries overwritten before they could be flushed

bool g_enabled = false;
FILE *g_file = nullptr;
uint64_t g_eventsWritten = 0;

// dt-since-previous-event-of-the-same-(type,layer) bookkeeping. Indexed by
// [DiagnosticEventType][CaptureLayer], both tiny fixed-cardinality enums, so
// a flat array is simplest -- no map, no allocation. -1.0 means "no previous
// event yet in this stream". Deliberately computed here at write time (not
// captured in DiagnosticEvent) -- it's a derived quantity from the sequence
// of raw timestamps, not a raw fact about a single event. Trace lines get a
// slot too (unused meaningfully, but keeps the indexing uniform).
constexpr int kEventTypeCount = 8;
constexpr int kLayerCount = 2;      // AppMonitor, ViewSwizzle
double g_lastTimestamp[kEventTypeCount][kLayerCount] = {}; // zero-init, then...

void ResetDtTracking() {
    for (int t = 0; t < kEventTypeCount; ++t)
        for (int l = 0; l < kLayerCount; ++l)
            g_lastTimestamp[t][l] = -1.0;
}

// Returns dt in seconds since the previous event of this exact (type, layer)
// stream, or -1.0 if this is the first one seen. Updates the tracked
// timestamp as a side effect (call exactly once per event, in log order).
double AdvanceAndComputeDt(DiagnosticEventType type, CaptureLayer layer, double timestamp) {
    double &last = g_lastTimestamp[(int)type][(int)layer];
    double dt = (last < 0.0) ? -1.0 : (timestamp - last);
    last = timestamp;
    return dt;
}

void FormatDt(double dt, char *out, size_t outSize) {
    if (dt < 0.0) {
        snprintf(out, outSize, "n/a");
    } else {
        snprintf(out, outSize, "%.6f", dt);
    }
}

inline bool OnMainThread() {
    return pthread_main_np() != 0;
}

// Names for the NSEventType values most useful to identify during the
// "is the monitor firing at all" diagnostic. Verified against AppKit's
// NSEvent.h (NS_ENUM(NSUInteger, NSEventType)) -- not guessed. Anything not
// listed here just prints its raw integer, which is always correct even if
// unnamed.
const char *RawEventTypeName(uint32_t t) {
    switch (t) {
        case 1: return "LeftMouseDown";
        case 2: return "LeftMouseUp";
        case 3: return "RightMouseDown";
        case 4: return "RightMouseUp";
        case 5: return "MouseMoved";
        case 6: return "LeftMouseDragged";
        case 7: return "RightMouseDragged";
        case 8: return "MouseEntered";
        case 9: return "MouseExited";
        case 10: return "KeyDown";
        case 11: return "KeyUp";
        case 12: return "FlagsChanged";
        case 13: return "AppKitDefined";
        case 14: return "SystemDefined";
        case 15: return "ApplicationDefined";
        case 16: return "Periodic";
        case 17: return "CursorUpdate";
        case 18: return "Rotate";
        case 19: return "BeginGesture";
        case 20: return "EndGesture";
        case 22: return "ScrollWheel";       // handled separately, shouldn't reach here
        case 23: return "TabletPoint";
        case 24: return "TabletProximity";
        case 25: return "OtherMouseDown";
        case 26: return "OtherMouseUp";
        case 27: return "OtherMouseDragged";
        case 29: return "Gesture";
        case 30: return "Magnify";           // handled separately, shouldn't reach here
        case 31: return "Swipe";
        case 32: return "SmartMagnify";
        case 33: return "QuickLook";
        case 34: return "Pressure";
        case 37: return "DirectTouch";
        case 38: return "ChangeMode";
        default: return "?";
    }
}

const char *LayerName(CaptureLayer layer) {
    switch (layer) {
        case CaptureLayer::AppMonitor: return "appmonitor";
        case CaptureLayer::ViewSwizzle: return "viewswizzle";
    }
    return "?";
}

const char *PhaseName(uint8_t phase) {
    if (phase & PhaseBegan) return "began";
    if (phase & PhaseChanged) return "changed";
    if (phase & PhaseEnded) return "ended";
    if (phase & PhaseCancelled) return "cancelled";
    if (phase & PhaseStationary) return "stationary";
    if (phase & PhaseMayBegin) return "maybegin";
    return "none";
}

const char *HierarchyRelationName(ViewHierarchyRelation relation) {
    switch (relation) {
        case ViewHierarchyRelation::Receiver: return "receiver";
        case ViewHierarchyRelation::Ancestor: return "ancestor";
        case ViewHierarchyRelation::Descendant: return "descendant";
    }
    return "?";
}

const char *SnapshotMomentName(ViewSnapshotMoment moment) {
    switch (moment) {
        case ViewSnapshotMoment::Begin: return "begin";
        case ViewSnapshotMoment::End: return "end";
    }
    return "?";
}

void FormatModifiers(uint32_t mods, char *out, size_t outSize) {
    out[0] = '\0';
    bool first = true;
    auto append = [&](const char *name) {
        if (!first) strncat(out, "+", outSize - strlen(out) - 1);
        strncat(out, name, outSize - strlen(out) - 1);
        first = false;
    };
    if (mods & ModCommand) append("cmd");
    if (mods & ModOption) append("opt");
    if (mods & ModControl) append("ctrl");
    if (mods & ModShift) append("shift");
    if (first) strncpy(out, "none", outSize - 1);
}

void WriteEventLine(const DiagnosticEvent &e) {
    // Trace lines are their own tiny format -- skip all the scroll/magnify
    // machinery (mods/dt/etc. below still get their bookkeeping advanced for
    // consistency, but aren't printed).
    double dt = AdvanceAndComputeDt(e.type, e.captureLayer, e.timestampSeconds);

    if (e.type == DiagnosticEventType::Trace) {
        fprintf(g_file, "%.6f [%s] %s\n", e.timestampSeconds,
                e.traceStage ? e.traceStage : "?", e.traceMessage ? e.traceMessage : "");
        return;
    }

    if (e.type == DiagnosticEventType::Momentum) {
        fprintf(g_file, "%.6f momentum amount=%.5f velocity=%.5f dt=%.6f\n",
                e.timestampSeconds, e.magnification, e.momentumVelocity, e.momentumDt);
        return;
    }

    if (e.type == DiagnosticEventType::VerticalCalibration) {
        fprintf(g_file,
                "%.6f vertical_calibration valid=%d before=%.3f after_plus=%.3f "
                "after_restore=%.3f delta_plus=%.3f restore_error=%.3f\n",
                e.timestampSeconds, e.calibrationValid ? 1 : 0,
                e.calibrationBeforeY, e.calibrationAfterPositiveY,
                e.calibrationAfterRestoreY,
                e.calibrationAfterPositiveY - e.calibrationBeforeY,
                e.calibrationAfterRestoreY - e.calibrationBeforeY);
        return;
    }

    if (e.type == DiagnosticEventType::MixerCalibration) {
        fprintf(g_file,
                "%.6f mixer_calibration MCP_before=%.3f MCP_after_plus=%.3f "
                "MCP_delta_plus=%.3f MCP_after_restore=%.3f MCP_restore_error=%.3f "
                "Arrange_before=%.9f Arrange_after_plus=%.9f "
                "Arrange_delta_plus=%.9f Arrange_after_restore=%.9f "
                "Arrange_restore_error=%.9f\n",
                e.timestampSeconds,
                e.mixerCalibrationBeforeX,
                e.mixerCalibrationAfterPositiveX,
                e.mixerCalibrationAfterPositiveX - e.mixerCalibrationBeforeX,
                e.mixerCalibrationAfterRestoreX,
                e.mixerCalibrationAfterRestoreX - e.mixerCalibrationBeforeX,
                e.mixerCalibrationBeforeArrange,
                e.mixerCalibrationAfterPositiveArrange,
                e.mixerCalibrationAfterPositiveArrange - e.mixerCalibrationBeforeArrange,
                e.mixerCalibrationAfterRestoreArrange,
                e.mixerCalibrationAfterRestoreArrange - e.mixerCalibrationBeforeArrange);
        return;
    }

    if (e.type == DiagnosticEventType::ViewHierarchy) {
        fprintf(g_file,
                "%.6f view_hierarchy moment=%s relation=%s depth=%u "
                "receiver=%#llx node=%#llx parent=%#llx class=%s tag=%lld "
                "frame=(%.1f,%.1f,%.1f,%.1f) bounds=(%.1f,%.1f,%.1f,%.1f) "
                "flipped=%d hidden=%d scrollview=%d clipview=%d enclosing_scroll=%#llx win=%d\n",
                e.timestampSeconds, SnapshotMomentName(e.hierarchyMoment),
                HierarchyRelationName(e.hierarchyRelation),
                (unsigned)e.hierarchyDepth,
                (unsigned long long)e.hierarchyReceiverId,
                (unsigned long long)e.hitViewId,
                (unsigned long long)e.hierarchyParentId,
                e.viewClassName[0] ? e.viewClassName : "-",
                (long long)e.hierarchyTag,
                e.hierarchyFrameX, e.hierarchyFrameY,
                e.hierarchyFrameWidth, e.hierarchyFrameHeight,
                e.hierarchyBoundsX, e.hierarchyBoundsY,
                e.hierarchyBoundsWidth, e.hierarchyBoundsHeight,
                e.hierarchyIsFlipped ? 1 : 0,
                e.hierarchyIsHidden ? 1 : 0,
                e.hierarchyIsScrollView ? 1 : 0,
                e.hierarchyIsClipView ? 1 : 0,
                (unsigned long long)e.hierarchyEnclosingScrollViewId,
                e.windowNumber);
        return;
    }

    char mods[64];
    FormatModifiers(e.modifiers, mods, sizeof(mods));

    const char *cls = e.viewClassName[0] ? e.viewClassName : "-";

    char dtStr[24];
    FormatDt(dt, dtStr, sizeof(dtStr));

    switch (e.type) {
        case DiagnosticEventType::Scroll:
            fprintf(g_file,
                "%.6f scroll dx=%.3f dy=%.3f pdx=%.3f pdy=%.3f precise=%d "
                "phase=%s momentum=%s mods=%s rawmods=%#llx dt=%s layer=%s class=%s win=%d view=%#llx loc=(%.1f,%.1f)\n",
                e.timestampSeconds, e.deltaX, e.deltaY, e.preciseDeltaX, e.preciseDeltaY,
                e.hasPreciseDeltas ? 1 : 0, PhaseName(e.phase), PhaseName(e.momentumPhase),
                mods, (unsigned long long)e.rawModifierFlags, dtStr,
                LayerName(e.captureLayer), cls, e.windowNumber,
                (unsigned long long)e.hitViewId, e.locationInWindowX, e.locationInWindowY);
            break;
        case DiagnosticEventType::Magnify:
            fprintf(g_file,
                "%.6f magnify mag=%.5f phase=%s mods=%s rawmods=%#llx dt=%s layer=%s class=%s win=%d view=%#llx loc=(%.1f,%.1f)\n",
                e.timestampSeconds, e.magnification, PhaseName(e.phase),
                mods, (unsigned long long)e.rawModifierFlags, dtStr,
                LayerName(e.captureLayer), cls, e.windowNumber,
                (unsigned long long)e.hitViewId, e.locationInWindowX, e.locationInWindowY);
            break;
        case DiagnosticEventType::Other:
            fprintf(g_file,
                "%.6f other type=%u(%s) mods=%s layer=%s win=%d view=%#llx loc=(%.1f,%.1f)\n",
                e.timestampSeconds, e.rawEventType, RawEventTypeName(e.rawEventType),
                mods, LayerName(e.captureLayer), e.windowNumber,
                (unsigned long long)e.hitViewId, e.locationInWindowX, e.locationInWindowY);
            break;
        case DiagnosticEventType::Trace:
        case DiagnosticEventType::Momentum:
        case DiagnosticEventType::VerticalCalibration:
        case DiagnosticEventType::ViewHierarchy:
        case DiagnosticEventType::MixerCalibration:
            break; // handled above, before the mods/dt work
    }
}

} // namespace

bool Init(const char *resourceDir) {
    assert(OnMainThread());
    ResetDtTracking();

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path logsDir = fs::path(resourceDir) / "logs";
    fs::create_directories(logsDir, ec); // ignores "already exists"

    time_t now = time(nullptr);
    struct tm tmv{};
    localtime_r(&now, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);

    fs::path logPath = logsDir / (std::string("session-") + stamp + ".log");

    g_file = fopen(logPath.c_str(), "w");
    if (!g_file) return false;

    setvbuf(g_file, nullptr, _IOFBF, 64 * 1024);
    fprintf(g_file,
        "# Trackpad Engine diagnostics log\n"
        "# scroll  : ts scroll dx=.. dy=.. pdx=.. pdy=.. precise=0/1 phase=.. momentum=.. mods=.. rawmods=.. dt=.. layer=.. class=.. win=.. view=.. loc=(x,y)\n"
        "# magnify : ts magnify mag=.. phase=.. mods=.. rawmods=.. dt=.. layer=.. class=.. win=.. view=.. loc=(x,y)\n"
        "# other   : ts other type=N(Name) mods=.. layer=.. win=.. view=.. loc=(x,y)   -- DIAGNOSTIC: every other NSEventType, see TrackpadInterceptor.mm\n"
        "# trace    : ts [Stage] message                                               -- Phase 2 pipeline wiring check, see Diagnostics::Trace()\n"
        "# momentum : ts momentum amount=.. velocity=.. dt=..                          -- synthesized pinch-zoom momentum tick, see PinchZoomProcessor::Tick()\n"
        "# vertical_calibration: ts vertical_calibration valid=.. before=.. after_plus=.. after_restore=.. delta_plus=.. restore_error=..\n"
        "# mixer_calibration: reversible CSurf_OnScroll(xdir,0) probe; records MCP X and Arrange start before/after/restore\n"
        "# view_hierarchy: gesture-boundary Cocoa node snapshot around the real receiver; frame/bounds are in parent/local coordinates\n"
        "# layer=appmonitor  : NSApplication local event monitor (TrackpadInterceptor.mm)\n"
        "# layer=viewswizzle : scrollWheel:/magnifyWithEvent: swizzled on a REAPER view class (ViewSwizzleInterceptor.mm); class= is that class's real runtime name\n"
        "# dx/dy       : NSEvent.deltaX/deltaY (line-based)\n"
        "# pdx/pdy     : NSEvent.scrollingDeltaX/scrollingDeltaY (precise, pixel-based when precise=1 i.e. hasPreciseScrollingDeltas)\n"
        "# mods        : decoded modifier keys (cmd/opt/ctrl/shift only, '+'-joined, or 'none')\n"
        "# rawmods     : NSEvent.modifierFlags, full raw bitmask, unfiltered (hex)\n"
        "# dt          : seconds since the previous logged event of the SAME event type + SAME layer (scroll vs magnify tracked separately); 'n/a' for the first one in this session\n"
        "# ts is NSEvent.timestamp (seconds since system boot) for scroll/magnify/other; trace lines use a separate monotonic clock (no NSEvent involved at those pipeline stages) so don't compare trace ts directly against scroll/magnify ts, only use dt/ordering within each.\n");
    fflush(g_file);
    return true;
}

void Shutdown() {
    assert(OnMainThread());
    Flush();
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void SetEnabled(bool enabled) { g_enabled = enabled; }
bool IsEnabled() { return g_enabled; }

void Capture(const DiagnosticEvent &event) {
    assert(OnMainThread());
    if (!g_enabled) return;

    g_buffer[g_writePos] = event; // POD copy, no allocation
    g_writePos = (g_writePos + 1) % kCapacity;

    if (g_size < kCapacity) {
        ++g_size;
    } else {
        // Ring is full: the slot we just wrote had the oldest unread entry,
        // which is now gone. Advance the read side past it and count the loss.
        g_readPos = (g_readPos + 1) % kCapacity;
        ++g_dropped;
    }
}

void Trace(const char *stage, const char *message) {
    assert(OnMainThread());
    if (!g_enabled) return;

    DiagnosticEvent e;
    e.type = DiagnosticEventType::Trace;
    e.captureLayer = CaptureLayer::ViewSwizzle; // only source of trace calls right now
    e.traceStage = stage;
    e.traceMessage = message;
    // Pure C++ monotonic clock -- no Cocoa/NSEvent involved at the
    // MotionEngine/ReaperNavigation stages, which is the whole point of the
    // Phase 2 split. Only meaningful for ordering/latency between trace
    // lines, not for comparing against NSEvent.timestamp's own base.
    e.timestampSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    Capture(e);
}

void Flush() {
    assert(OnMainThread());
    if (!g_file) return;

    bool wroteAnything = false;

    if (g_size > 0) {
        size_t n = g_size;
        for (size_t i = 0; i < n; ++i) {
            WriteEventLine(g_buffer[g_readPos]);
            g_readPos = (g_readPos + 1) % kCapacity;
        }
        g_size -= n;
        g_eventsWritten += n;
        wroteAnything = true;
    }

    if (g_dropped > 0) {
        fprintf(g_file, "# WARNING: %llu events dropped (ring buffer overrun)\n",
                (unsigned long long)g_dropped);
        g_dropped = 0;
        wroteAnything = true;
    }

    if (wroteAnything) {
        fflush(g_file);
    }
}

} // namespace Diagnostics
