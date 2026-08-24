#include "HorizontalScrollProcessor.h"

namespace HorizontalScrollProcessor {
namespace {

double g_sensitivity = 1.0; // neutral default: output == input delta

} // namespace

double Process(const TrackpadEvent &event) {
    if (event.type != TrackpadEventType::Scroll) return 0.0;

    // This project targets the Apple trackpad exclusively (see README).
    // Non-precise deltas come from actual mouse wheels or older/other
    // devices, not the two-finger trackpad gesture this engine is for --
    // treated as "no usable data" rather than guessing a line-height
    // conversion that would apply to a different kind of input entirely.
    if (!event.hasPreciseDeltas) return 0.0;

    return event.preciseDeltaX * g_sensitivity;
}

void SetSensitivity(double sensitivity) { g_sensitivity = sensitivity; }
double GetSensitivity() { return g_sensitivity; }

} // namespace HorizontalScrollProcessor
