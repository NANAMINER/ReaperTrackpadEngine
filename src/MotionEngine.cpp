#include "MotionEngine.h"

#include "Diagnostics.h"
#include "PinchZoomProcessor.h"
#include "ScrollGestureProcessor.h"

namespace MotionEngine {

NavigationCommand ProcessEvent(const TrackpadEvent &event) {
    Diagnostics::Trace("MotionEngine", "received TrackpadEvent");

    NavigationCommand cmd;
    if (event.type == TrackpadEventType::Scroll) {
        cmd = ScrollGestureProcessor::Process(event);
    } else if (event.type == TrackpadEventType::Magnify) {
        cmd.zoomAmount = PinchZoomProcessor::Process(event);
    }
    // trackHeightDelta stays at its zeroed default -- not implemented yet.
    return cmd;
}

NavigationCommand Tick(TrackpadEvent &outContext) {
    NavigationCommand cmd;
    cmd.zoomAmount = PinchZoomProcessor::Tick(outContext);
    return cmd;
}

} // namespace MotionEngine
