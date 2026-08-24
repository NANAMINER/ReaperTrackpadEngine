#pragma once

#include "NavigationCommand.h"
#include "TrackpadEvent.h"

// Pure C++ motion-processing stage: TrackpadEvent in, NavigationCommand out.
// Must never depend on Cocoa/AppKit/NSEvent or on the REAPER API -- it should
// be possible to build and unit-test this file in complete isolation from
// both REAPER and macOS frameworks.
//
// Scroll gestures: per-event 2D routing (ScrollGestureProcessor), then
// horizontal pixel pass-through and/or calibrated vertical accumulation.
// Pinch zoom: first-version pass-through math plus synthesized momentum
// (PinchZoomProcessor) -- macOS doesn't generate momentum events for pinch
// the way it does for scroll, so that inertia is synthesized here instead of
// just relayed from the OS. See PinchZoomProcessor.h.
namespace MotionEngine {

// Called once per real TrackpadEvent (from ViewSwizzleInterceptor).
NavigationCommand ProcessEvent(const TrackpadEvent &event);

// Called once per REAPER timer tick (see PluginEntry.cpp's TimerTick), NOT
// from a Cocoa event -- advances any active synthesized momentum (currently
// pinch-zoom only) by real elapsed time since the last call. outContext is
// filled with the cached anchor/view context from the gesture that started
// the momentum (there's no live NSEvent during a tick); pass it straight to
// ReaperNavigation::Apply exactly like a real event's TrackpadEvent would be.
// Returns a zeroed NavigationCommand when no momentum is active (the common
// case -- nothing to do).
NavigationCommand Tick(TrackpadEvent &outContext);

} // namespace MotionEngine
