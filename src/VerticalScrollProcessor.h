#pragma once

#include "TrackpadEvent.h"

// Converts the trackpad's precise vertical pixel delta into the integer ydir
// units required by REAPER's CSurf_OnScroll. Live calibration established
// that one ydir unit moves the Arrange/TCP view by exactly 8 pixels.
namespace VerticalScrollProcessor {

// Returns an integer-valued double suitable for NavigationCommand. Fractional
// pixels are retained across events. The final 1 px/event momentum tail is
// stopped before it can accumulate into isolated, visibly quantized steps.
double Process(const TrackpadEvent &event);

// Starts a new gesture with no remainder from the previous one.
void Reset();

void SetSensitivity(double sensitivity);
double GetSensitivity();

} // namespace VerticalScrollProcessor
