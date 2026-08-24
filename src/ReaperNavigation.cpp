// REAPERAPI_MINIMAL + REAPERAPI_WANT_* here must mirror what's requested,
// since PluginEntry.cpp does NOT define REAPERAPI_IMPLEMENT for this file --
// only PluginEntry.cpp allocates storage for these function pointers (its
// own REAPERAPI_WANT_* list must include the same names, or these `extern`
// declarations won't resolve at link time). See PluginEntry.cpp's top
// comment.
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_CSurf_OnScroll
#define REAPERAPI_WANT_GetMediaTrackInfo_Value
#define REAPERAPI_WANT_GetSet_ArrangeView2
#define REAPERAPI_WANT_GetCursorPositionEx
#define REAPERAPI_WANT_GetMixerScroll
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_UpdateTimeline
#include "reaper_plugin_functions.h"

#include "ReaperNavigation.h"

#include "Diagnostics.h"

#include <cmath>

namespace ReaperNavigation {
namespace {

bool g_horizontalScrollEnabled = false;
bool g_verticalScrollEnabled = false;
bool g_zoomEnabled = false;

enum class VerticalCalibrationPhase {
    Idle,
    MeasureBaseline,
    MeasurePositive,
    MeasureRestore,
};

VerticalCalibrationPhase g_verticalCalibrationPhase = VerticalCalibrationPhase::Idle;
MediaTrack *g_verticalCalibrationTrack = nullptr;
VerticalScrollCalibrationResult g_verticalCalibrationResult;

enum class HorizontalCalibrationPhase {
    Idle,
    MeasureBaseline,
    MeasurePositive,
    MeasureRestore,
};

HorizontalCalibrationPhase g_horizontalCalibrationPhase =
    HorizontalCalibrationPhase::Idle;
MediaTrack *g_horizontalCalibrationTrack = nullptr;
HorizontalScrollCalibrationResult g_horizontalCalibrationResult;
uintptr_t g_trackControlPanelReceiverId = 0;
uintptr_t g_mixerPanelReceiverId = 0;
uintptr_t g_pendingMixerReceiverId = 0;
ViewportSnapshot g_pendingMixerSnapshot;
bool g_hasPendingMixerSnapshot = false;

double ReadArrangeStart() {
    double start = 0.0, end = 0.0;
    GetSet_ArrangeView2(nullptr, false, 0, 0, &start, &end);
    return start;
}

// Returns true if REAPER's horizontal view position was actually changed.
bool ApplyHorizontalScroll(const TrackpadEvent &event, double deltaPixels) {
    if (deltaPixels == 0.0) return false; // nothing requested

    if (event.viewWidthPixels <= 0.0) return false; // missing data

    if (!event.isInArrangeView) return false;

    double start = 0.0, end = 0.0;
    GetSet_ArrangeView2(nullptr, false, 0, 0, &start, &end);

    double visibleDuration = end - start;
    if (visibleDuration <= 0.0) return false; // sanity guard: degenerate/unexpected state

    double pixelsPerSecond = event.viewWidthPixels / visibleDuration;
    // Confirmed inverted by testing: the direct reading (newStart = start +
    // deltaSeconds) scrolled backwards relative to REAPER's native
    // direction. Negated here so a rightward swipe matches native REAPER
    // behavior (content follows the fingers).
    double deltaSeconds = -deltaPixels / pixelsPerSecond;

    double newStart = start + deltaSeconds;
    double newEnd = end + deltaSeconds;
    GetSet_ArrangeView2(nullptr, true, 0, 0, &newStart, &newEnd);

    // Verify empirically whether this is even necessary, or GetSet_ArrangeView2
    // already triggers its own redraw -- not documented either way.
    UpdateTimeline();

    return true;
}

bool ApplyVerticalScroll(const TrackpadEvent &event, double stepAmount) {
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

// Pinch-zoom, anchored on REAPER's edit cursor: read the current visible
// span, compute where (as a 0..1 fraction) the edit cursor sits within it,
// shrink/grow the span around that fraction, write it back. Reuses
// GetSet_ArrangeView2 exactly like horizontal scroll -- no separate zoom API
// needed (adjustZoom() exists but its centermode parameter isn't documented
// well enough to guarantee edit-cursor anchoring, so this computes the
// anchor ourselves instead of trusting it).
//
// zoomAmount is a relative scale delta using NSEvent.magnification's own
// convention (Apple's documented formula: scaleFactor = 1.0 + magnification).
// Direction is confirmed empirically: pinch-out (spread fingers, positive
// magnification) zooms IN (shows less time), hence newDuration = duration /
// scaleFactor.
bool ApplyZoom(const TrackpadEvent &event, double zoomAmount) {
    if (zoomAmount == 0.0) return false; // nothing requested

    if (!event.isInArrangeView) return false;

    double start = 0.0, end = 0.0;
    GetSet_ArrangeView2(nullptr, false, 0, 0, &start, &end);

    double duration = end - start;
    if (duration <= 0.0) return false; // sanity guard: degenerate/unexpected state

    double scaleFactor = 1.0 + zoomAmount;
    // Defensive only -- real per-event trackpad magnification deltas are
    // small, this guards against a pathological/corrupted value collapsing
    // or inverting the view rather than any expected input.
    if (scaleFactor <= 0.01) return false;

    double newDuration = duration / scaleFactor;

    // Anchor on REAPER's edit cursor (the vertical timeline cursor), not the
    // macOS mouse pointer. Keeping its current fractional screen position
    // fixed avoids a visual jump even when it is not centered in the view.
    double anchorTime = GetCursorPositionEx(nullptr);
    double fraction = (anchorTime - start) / duration;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;

    // If the edit cursor is outside the visible span, anchor to the nearest
    // visible edge rather than extrapolating the Arrange view off-screen.
    anchorTime = start + fraction * duration;
    double newStart = anchorTime - fraction * newDuration;
    double newEnd = newStart + newDuration;

    GetSet_ArrangeView2(nullptr, true, 0, 0, &newStart, &newEnd);

    // No explicit clamp against REAPER's own min/max zoom (e.g.
    // ARRANGE_MIN_TIMESCALE) -- relying on GetSet_ArrangeView2's own internal
    // validation rather than reimplementing bounds we don't have a
    // documented way to read from here. Verify with a fast/extreme pinch
    // during testing.
    UpdateTimeline();

    return true;
}

} // namespace

void SetHorizontalScrollEnabled(bool enabled) { g_horizontalScrollEnabled = enabled; }
bool IsHorizontalScrollEnabled() { return g_horizontalScrollEnabled; }
void SetVerticalScrollEnabled(bool enabled) { g_verticalScrollEnabled = enabled; }
bool IsVerticalScrollEnabled() { return g_verticalScrollEnabled; }
void SetZoomEnabled(bool enabled) { g_zoomEnabled = enabled; }
bool IsZoomEnabled() { return g_zoomEnabled; }

bool BeginVerticalScrollCalibration() {
    if (g_verticalCalibrationPhase != VerticalCalibrationPhase::Idle) return false;

    g_verticalCalibrationTrack = nullptr;
    g_verticalCalibrationResult = {};
    g_verticalCalibrationPhase = VerticalCalibrationPhase::MeasureBaseline;
    return true;
}

bool TickVerticalScrollCalibration(VerticalScrollCalibrationResult &result) {
    switch (g_verticalCalibrationPhase) {
        case VerticalCalibrationPhase::Idle:
            return false;

        case VerticalCalibrationPhase::MeasureBaseline:
            g_verticalCalibrationTrack = GetTrack(nullptr, 0);
            if (!g_verticalCalibrationTrack) {
                result = {};
                g_verticalCalibrationPhase = VerticalCalibrationPhase::Idle;
                return true;
            }

            g_verticalCalibrationResult.beforeY =
                GetMediaTrackInfo_Value(g_verticalCalibrationTrack, "I_TCPY");
            CSurf_OnScroll(0, 1);
            g_verticalCalibrationPhase = VerticalCalibrationPhase::MeasurePositive;
            return false;

        case VerticalCalibrationPhase::MeasurePositive:
            g_verticalCalibrationResult.afterPositiveY =
                GetMediaTrackInfo_Value(g_verticalCalibrationTrack, "I_TCPY");

            // At a scroll boundary +1 can be a no-op. Do not issue -1 in
            // that case: it would move away from the boundary rather than
            // restore the original position. The zero delta tells the user
            // to move to the middle and rerun the probe.
            if (g_verticalCalibrationResult.afterPositiveY ==
                g_verticalCalibrationResult.beforeY) {
                g_verticalCalibrationResult.afterRestoreY =
                    g_verticalCalibrationResult.beforeY;
                g_verticalCalibrationResult.valid = true;
                result = g_verticalCalibrationResult;
                g_verticalCalibrationTrack = nullptr;
                g_verticalCalibrationPhase = VerticalCalibrationPhase::Idle;
                return true;
            }

            CSurf_OnScroll(0, -1);
            g_verticalCalibrationPhase = VerticalCalibrationPhase::MeasureRestore;
            return false;

        case VerticalCalibrationPhase::MeasureRestore:
            g_verticalCalibrationResult.afterRestoreY =
                GetMediaTrackInfo_Value(g_verticalCalibrationTrack, "I_TCPY");
            g_verticalCalibrationResult.valid = true;
            result = g_verticalCalibrationResult;
            g_verticalCalibrationTrack = nullptr;
            g_verticalCalibrationPhase = VerticalCalibrationPhase::Idle;
            return true;
    }

    return false;
}

bool BeginHorizontalScrollCalibration() {
    if (g_horizontalCalibrationPhase != HorizontalCalibrationPhase::Idle) return false;

    g_horizontalCalibrationTrack = nullptr;
    g_horizontalCalibrationResult = {};
    g_horizontalCalibrationPhase = HorizontalCalibrationPhase::MeasureBaseline;
    return true;
}

bool TickHorizontalScrollCalibration(HorizontalScrollCalibrationResult &result) {
    switch (g_horizontalCalibrationPhase) {
        case HorizontalCalibrationPhase::Idle:
            return false;

        case HorizontalCalibrationPhase::MeasureBaseline:
            g_horizontalCalibrationTrack = GetMixerScroll();
            if (!g_horizontalCalibrationTrack) {
                result = {};
                g_horizontalCalibrationPhase = HorizontalCalibrationPhase::Idle;
                return true;
            }

            g_horizontalCalibrationResult.beforeMcpX =
                GetMediaTrackInfo_Value(g_horizontalCalibrationTrack, "I_MCPX");
            g_horizontalCalibrationResult.beforeArrangeStart = ReadArrangeStart();
            CSurf_OnScroll(1, 0);
            g_horizontalCalibrationPhase = HorizontalCalibrationPhase::MeasurePositive;
            return false;

        case HorizontalCalibrationPhase::MeasurePositive: {
            g_horizontalCalibrationResult.afterPositiveMcpX =
                GetMediaTrackInfo_Value(g_horizontalCalibrationTrack, "I_MCPX");
            g_horizontalCalibrationResult.afterPositiveArrangeStart = ReadArrangeStart();

            bool movedMcp = g_horizontalCalibrationResult.afterPositiveMcpX !=
                            g_horizontalCalibrationResult.beforeMcpX;
            bool movedArrange = g_horizontalCalibrationResult.afterPositiveArrangeStart !=
                                g_horizontalCalibrationResult.beforeArrangeStart;
            if (!movedMcp && !movedArrange) {
                g_horizontalCalibrationResult.afterRestoreMcpX =
                    g_horizontalCalibrationResult.beforeMcpX;
                g_horizontalCalibrationResult.afterRestoreArrangeStart =
                    g_horizontalCalibrationResult.beforeArrangeStart;
                g_horizontalCalibrationResult.valid = true;
                result = g_horizontalCalibrationResult;
                g_horizontalCalibrationTrack = nullptr;
                g_horizontalCalibrationPhase = HorizontalCalibrationPhase::Idle;
                return true;
            }

            CSurf_OnScroll(-1, 0);
            g_horizontalCalibrationPhase = HorizontalCalibrationPhase::MeasureRestore;
            return false;
        }

        case HorizontalCalibrationPhase::MeasureRestore:
            g_horizontalCalibrationResult.afterRestoreMcpX =
                GetMediaTrackInfo_Value(g_horizontalCalibrationTrack, "I_MCPX");
            g_horizontalCalibrationResult.afterRestoreArrangeStart = ReadArrangeStart();
            g_horizontalCalibrationResult.valid = true;
            result = g_horizontalCalibrationResult;
            g_horizontalCalibrationTrack = nullptr;
            g_horizontalCalibrationPhase = HorizontalCalibrationPhase::Idle;
            return true;
    }

    return false;
}

ViewportSnapshot CaptureViewportSnapshot() {
    ViewportSnapshot snapshot;

    if (MediaTrack *firstTrack = GetTrack(nullptr, 0)) {
        snapshot.hasTcp = true;
        snapshot.tcpY = GetMediaTrackInfo_Value(firstTrack, "I_TCPY");
    }

    if (MediaTrack *mixerTrack = GetMixerScroll()) {
        snapshot.hasMixer = true;
        snapshot.mixerTrackId = reinterpret_cast<uintptr_t>(mixerTrack);
        snapshot.mixerTrackX = GetMediaTrackInfo_Value(mixerTrack, "I_MCPX");
    }

    return snapshot;
}

void ObserveNativeScroll(const TrackpadEvent &event,
                         const ViewportSnapshot &before,
                         const ViewportSnapshot &after) {
    if (!event.receiverId || event.isInArrangeView || !event.hasPreciseDeltas) return;

    double absX = std::abs(event.preciseDeltaX);
    double absY = std::abs(event.preciseDeltaY);
    bool verticalIntent = absY > absX * 3.0;
    bool horizontalIntent = absX > absY * 3.0;

    if (verticalIntent && before.hasTcp && after.hasTcp && before.tcpY != after.tcpY) {
        if (g_trackControlPanelReceiverId != event.receiverId) {
            g_trackControlPanelReceiverId = event.receiverId;
            Diagnostics::Trace("TrackpadTarget", "learned view: tcp");
        }
    }

    auto mixerStateChanged = [](const ViewportSnapshot &a,
                                const ViewportSnapshot &b) {
        return a.hasMixer && b.hasMixer &&
            (a.mixerTrackId != b.mixerTrackId ||
             a.mixerTrackX != b.mixerTrackX);
    };

    // REAPER can commit MCP scrolling after scrollWheel: returns. Compare
    // both the immediate before/after pair and the previous event's "after"
    // against this event's "before" so that deferred UI updates still
    // identify the correct receiver.
    bool mixerChanged = mixerStateChanged(before, after);
    if (horizontalIntent && g_hasPendingMixerSnapshot &&
        g_pendingMixerReceiverId == event.receiverId &&
        mixerStateChanged(g_pendingMixerSnapshot, before)) {
        mixerChanged = true;
    }
    if (horizontalIntent && mixerChanged) {
        if (g_mixerPanelReceiverId != event.receiverId) {
            g_mixerPanelReceiverId = event.receiverId;
            Diagnostics::Trace("TrackpadTarget", "learned view: mixer");
        }
    }

    if (horizontalIntent) {
        g_pendingMixerReceiverId = event.receiverId;
        g_pendingMixerSnapshot = after;
        g_hasPendingMixerSnapshot = true;
    }
}

bool IsKnownTrackControlPanel(uintptr_t receiverId) {
    return receiverId != 0 && receiverId == g_trackControlPanelReceiverId;
}

bool IsKnownMixerPanel(uintptr_t receiverId) {
    return receiverId != 0 && receiverId == g_mixerPanelReceiverId;
}

bool NeedsSurfaceObservation(const TrackpadEvent &event) {
    if (!event.receiverId || event.isInArrangeView || !event.hasPreciseDeltas) return false;

    double absX = std::abs(event.preciseDeltaX);
    double absY = std::abs(event.preciseDeltaY);
    if (absY > absX * 3.0) return !IsKnownTrackControlPanel(event.receiverId);
    if (absX > absY * 3.0) return !IsKnownMixerPanel(event.receiverId);
    return false;
}

bool Apply(const TrackpadEvent &event, const NavigationCommand &command) {
    Diagnostics::Trace("Navigation", "received NavigationCommand");

    bool handledScroll = false;

    if (command.horizontalScroll != 0.0) {
        if (g_horizontalScrollEnabled) {
            bool changed = ApplyHorizontalScroll(event, command.horizontalScroll);
            if (changed) Diagnostics::Trace("Navigation", "applied horizontal scroll");
            handledScroll = handledScroll || changed;
        }
    }
    if (command.verticalScrollActive) {
        if (g_verticalScrollEnabled &&
            (event.isInArrangeView || event.isInTrackControlPanel)) {
            // Consume every event routed vertically -- including zero-output
            // events that only accumulate a sub-step remainder or belong to
            // the stopped momentum tail. Letting them fall through alternates
            // native and custom motion.
            if (command.verticalScroll != 0.0) {
                bool changed = ApplyVerticalScroll(event, command.verticalScroll);
                if (changed) Diagnostics::Trace("Navigation", "applied vertical scroll");
            }
            handledScroll = true;
        }
    }
    if (handledScroll) return true;

    if (command.zoomAmount != 0.0) {
        if (!g_zoomEnabled) return false;
        bool changed = ApplyZoom(event, command.zoomAmount);
        if (changed) Diagnostics::Trace("Navigation", "applied zoom");
        return changed;
    }
    // trackHeightDelta: not implemented yet.
    return false;
}

} // namespace ReaperNavigation
