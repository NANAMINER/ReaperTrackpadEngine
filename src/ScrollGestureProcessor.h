#pragma once

#include "NavigationCommand.h"
#include "TrackpadEvent.h"

// Routes every precise scroll event dynamically as horizontal, vertical, or
// two-axis motion. Narrow dead zones near the cardinal axes suppress ordinary
// cross-axis noise; no gesture-level lock exists, so curves and circles can
// change direction continuously through fingers-down motion and momentum.
namespace ScrollGestureProcessor {

NavigationCommand Process(const TrackpadEvent &event);

} // namespace ScrollGestureProcessor
