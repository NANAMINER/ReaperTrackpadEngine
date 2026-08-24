#pragma once

#include "TrackpadEvent.h"

// Pinch-zoom processing, now with synthesized momentum.
//
// Why momentum is needed here but not for horizontal scroll: NSEventTypeScroll
// has a real momentumPhase -- macOS itself keeps generating decaying scroll
// events after the fingers lift, so even a raw pass-through "feels" like it
// has inertia. NSEventTypeMagnify has NO equivalent: gesture events stop
// dead the instant the fingers stop moving (confirmed via TrackpadEvent.h,
// where momentumPhase is scroll-only). A raw pass-through for zoom therefore
// always looks like it "slams to a stop" -- not a bug, a platform gesture
// model difference. This module synthesizes the missing inertia ourselves.
//
// Model (as originally specified): track velocity while the gesture is
// active; on release, decay it exponentially (velocity *= exp(-decay*dt))
// each timer tick until it drops below a stop threshold. No response curve
// yet -- still first-version math otherwise.
namespace PinchZoomProcessor {

// Call once per real NSEventTypeMagnify TrackpadEvent (from
// MotionEngine::ProcessEvent). Returns the immediate zoom delta for THIS
// event (0.0 if not a Magnify event) -- same as before momentum was added.
// Also updates internal velocity tracking. PhaseEnded seeds synthesized
// momentum through a smooth speed-dependent release curve (near-zero for a
// slow pinch, up to 40% for a fast one), with a small additional boost when
// the gesture is both short and fast; PhaseCancelled stops without momentum.
// Terminal deltas are still returned for immediate application but never
// overwrite tracked velocity.
double Process(const TrackpadEvent &event);

// Call once per REAPER timer tick (see MotionEngine::Tick /
// PluginEntry.cpp's TimerTick). Advances any active momentum by real
// elapsed time since the previous Tick() call and returns this tick's zoom
// delta (0.0 if momentum isn't active -- the common case).
//
// outContext is filled with the cached Arrange-view context from the gesture
// that started the momentum -- there's no live NSEvent during a tick, so this
// is how ReaperNavigation::Apply gets the same context it would from a real event.
// Left untouched if this call returns 0.0.
double Tick(TrackpadEvent &outContext);

bool IsMomentumActive();

// Internal parameters (no GUI/config yet). Defaults are first guesses, not
// verified against real testing -- expect to retune once momentum has
// actually been tried live.
void SetSensitivity(double sensitivity); // 1.0 = pass raw trackpad magnification straight through
double GetSensitivity();
void SetDecay(double decayPerSecond);    // higher = momentum dies out faster
double GetDecay();
void SetStopThreshold(double threshold); // velocity magnitude below which momentum is considered stopped
double GetStopThreshold();

} // namespace PinchZoomProcessor
