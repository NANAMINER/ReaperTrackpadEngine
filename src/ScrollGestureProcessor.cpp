#include "ScrollGestureProcessor.h"

#include "HorizontalScrollProcessor.h"
#include "VerticalScrollProcessor.h"

#include <cmath>

namespace ScrollGestureProcessor {
namespace {

enum class RoutingMode {
    Undecided,
    Horizontal,
    Vertical,
    Diagonal,
};

// Re-evaluated for every event. Only suppress a secondary component when the
// primary component is at least three times larger; otherwise route both.
// The narrow mono-axis dead zones remove ordinary trackpad noise while the
// wide two-axis region allows continuous curves and circular motion.
constexpr double kDominanceRatio = 3.0;

void ResetGesture() {
    VerticalScrollProcessor::Reset();
}

RoutingMode Classify(const TrackpadEvent &event) {
    double x = std::fabs(event.preciseDeltaX);
    double y = std::fabs(event.preciseDeltaY);

    if (x == 0.0 && y == 0.0) {
        return RoutingMode::Undecided;
    }
    if (x > y * kDominanceRatio) return RoutingMode::Horizontal;
    if (y > x * kDominanceRatio) return RoutingMode::Vertical;
    return RoutingMode::Diagonal;
}

} // namespace

NavigationCommand Process(const TrackpadEvent &event) {
    NavigationCommand command;
    if (event.type != TrackpadEventType::Scroll || !event.hasPreciseDeltas) {
        return command;
    }

    if (event.phase & (PhaseMayBegin | PhaseBegan)) ResetGesture();

    if (event.phase & PhaseCancelled) {
        ResetGesture();
        return command;
    }

    RoutingMode mode = Classify(event);

    if (mode == RoutingMode::Horizontal) {
        command.horizontalScroll = HorizontalScrollProcessor::Process(event);
    } else if (mode == RoutingMode::Vertical) {
        command.verticalScrollActive = true;
        command.verticalScroll = VerticalScrollProcessor::Process(event);
    } else if (mode == RoutingMode::Diagonal) {
        command.horizontalScroll = HorizontalScrollProcessor::Process(event);
        command.verticalScrollActive = true;
        command.verticalScroll = VerticalScrollProcessor::Process(event);
    }

    // Native momentum carries its own X/Y vector and passes through the same
    // per-event router. Reset the vertical remainder when that stream ends;
    // the next Began/MayBegin also resets it.
    if (event.momentumPhase & PhaseEnded) ResetGesture();

    return command;
}

} // namespace ScrollGestureProcessor
