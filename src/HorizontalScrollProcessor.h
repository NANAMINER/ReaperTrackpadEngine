#pragma once

#include "TrackpadEvent.h"

// First-version horizontal scroll processing: no response curve, no
// smoothing, no velocity/momentum modeling yet -- just the trackpad's own
// precise pixel delta, scaled by a sensitivity multiplier.
//
// This is its own module (rather than inline in MotionEngine) so the real
// algorithm can replace its internals later without touching MotionEngine's
// wiring or anything downstream.
//
// Output stays in PIXELS (Arrange-view-local), NOT seconds -- pixel/time
// conversion depends on REAPER's current zoom level and view geometry,
// which belongs to ReaperNavigation, not here (see TrackpadEvent.h /
// ReaperNavigation.cpp).
namespace HorizontalScrollProcessor {

// Returns 0.0 for anything that isn't a usable scroll gesture (wrong event
// type, non-precise deltas -- see .cpp for why that's treated as "no data"
// rather than guessed).
double Process(const TrackpadEvent &event);

// Internal parameter (no GUI/config yet -- see project notes). 1.0 = pass
// the raw trackpad delta straight through.
void SetSensitivity(double sensitivity);
double GetSensitivity();

} // namespace HorizontalScrollProcessor
