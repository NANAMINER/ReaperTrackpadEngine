#pragma once

#include "NavigationCommand.h"
#include "TrackpadEvent.h"

// The only module allowed to talk to the REAPER API to apply
// scroll/zoom/track-height changes. Nothing upstream (ViewSwizzleInterceptor,
// MotionEngine) calls REAPER directly -- that responsibility lives here on
// purpose, so the rest of the pipeline stays swappable/testable independent
// of REAPER's actual API surface.
//
// Current scope: horizontal Arrange-view scrolling (GetSet_ArrangeView2),
// calibrated vertical scrolling (CSurf_OnScroll), and pinch-to-zoom anchored
// on REAPER's edit cursor (GetCursorPositionEx plus GetSet_ArrangeView2).
// Track height is not implemented; command.trackHeightDelta is ignored.
namespace ReaperNavigation {

// Independent kill switches, one per REAPER-affecting behavior. They default
// to false on first installation, then PluginEntry restores their persistent
// REAPER ExtState values. A master action controls all three together while
// the individual actions remain available for diagnostics/selective fallback.
void SetHorizontalScrollEnabled(bool enabled);
bool IsHorizontalScrollEnabled();
void SetVerticalScrollEnabled(bool enabled);
bool IsVerticalScrollEnabled();
void SetZoomEnabled(bool enabled);
bool IsZoomEnabled();

// One-shot empirical probe for REAPER's undocumented CSurf_OnScroll ydir
// units. Begin arms a three-timer-tick state machine: read the first track's
// I_TCPY, call ydir=+1, read again, then call ydir=-1 and verify restoration.
// Tick returns true exactly once when a result (valid or failed) is ready.
struct VerticalScrollCalibrationResult {
    bool valid = false;
    double beforeY = 0.0;
    double afterPositiveY = 0.0;
    double afterRestoreY = 0.0;
};

// Reversible one-step probe for the undocumented xdir parameter of
// CSurf_OnScroll. Both MCP position and Arrange start are measured so the
// result tells us which surface the call actually controls.
struct HorizontalScrollCalibrationResult {
    bool valid = false;
    double beforeMcpX = 0.0;
    double afterPositiveMcpX = 0.0;
    double afterRestoreMcpX = 0.0;
    double beforeArrangeStart = 0.0;
    double afterPositiveArrangeStart = 0.0;
    double afterRestoreArrangeStart = 0.0;
};

bool BeginVerticalScrollCalibration();
bool TickVerticalScrollCalibration(VerticalScrollCalibrationResult &result);
bool BeginHorizontalScrollCalibration();
bool TickHorizontalScrollCalibration(HorizontalScrollCalibrationResult &result);

// Lightweight before/after state used to learn REAPER's custom SWELL
// surfaces from their actual effect: a vertical native event changing TCPY
// identifies the TCP receiver; a horizontal one changing MCPX/leftmost track
// identifies the Mixer receiver.
struct ViewportSnapshot {
    bool hasTcp = false;
    double tcpY = 0.0;
    bool hasMixer = false;
    uintptr_t mixerTrackId = 0;
    double mixerTrackX = 0.0;
};

ViewportSnapshot CaptureViewportSnapshot();
void ObserveNativeScroll(const TrackpadEvent &event,
                         const ViewportSnapshot &before,
                         const ViewportSnapshot &after);
bool IsKnownTrackControlPanel(uintptr_t receiverId);
bool IsKnownMixerPanel(uintptr_t receiverId);
bool NeedsSurfaceObservation(const TrackpadEvent &event);

// Attempts to apply `command` to REAPER's state using context from `event`
// (Arrange-view membership and view geometry -- see TrackpadEvent.h).
// Arrange membership comes from the actual Cocoa receiving-
// view hierarchy; no coordinate hit test is performed here.
//
// Returns true if REAPER's state was actually changed -- the caller
// (ViewSwizzleInterceptor) must then suppress REAPER's own native handling
// for this event (do not call through to the original implementation), or
// the result is a double scroll/zoom.
//
// Returns false if nothing was done, for any of: the relevant switch is
// disabled, the event isn't over the Arrange view, required data is missing,
// or an internal sanity check failed. The caller must call through to
// REAPER's original handling in that case -- native behavior stays exactly
// as before.
bool Apply(const TrackpadEvent &event, const NavigationCommand &command);

} // namespace ReaperNavigation
