#pragma once

// Abstract output of the motion pipeline: WHAT should conceptually happen to
// REAPER's view, never HOW to make it happen. ReaperNavigation is the only
// module that knows how to turn this into actual REAPER API calls.
//
// Pure C++, no Cocoa/REAPER-API dependency -- MotionEngine produces this
// without knowing anything about how REAPER will apply it.
//
// Units: horizontalScroll is Arrange-view pixels, verticalScroll is an
// integer-valued CSurf_OnScroll ydir count (8 px per unit, live-calibrated),
// zoomAmount is a relative scale delta. verticalScrollActive remains true
// even when the current event only adds a sub-step remainder: the adapter
// must still suppress REAPER's native handler for that routed vertical event.
// trackHeightDelta is still unused.
struct NavigationCommand {
    double horizontalScroll = 0.0;
    double verticalScroll = 0.0;
    bool verticalScrollActive = false;
    double zoomAmount = 0.0;
    double trackHeightDelta = 0.0;
};
