#include "VerticalScrollProcessor.h"

#include <cmath>

namespace VerticalScrollProcessor {
namespace {

constexpr double kPixelsPerReaperStep = 8.0;
constexpr double kMomentumStopPixels = 1.0;
double g_sensitivity = 1.0;
double g_remainderPixels = 0.0;
bool g_momentumStopped = false;

} // namespace

double Process(const TrackpadEvent &event) {
    if (event.type != TrackpadEventType::Scroll) return 0.0;
    if (!event.hasPreciseDeltas) return 0.0;

    // CSurf_OnScroll cannot express less than one calibrated 8 px step. At
    // the very end of native momentum, 1 px events would otherwise build up
    // slowly and emit one isolated final step. Once the tail enters that
    // range, discard the remainder and consume it silently. Keeping 2-3 px
    // events preserves more of the natural deceleration than the earlier
    // 3 px cutoff, whose stop felt slightly abrupt in live testing.
    if (g_momentumStopped) return 0.0;
    if ((event.momentumPhase & (PhaseBegan | PhaseChanged)) &&
        std::fabs(event.preciseDeltaY) <= kMomentumStopPixels) {
        g_momentumStopped = true;
        g_remainderPixels = 0.0;
        return 0.0;
    }

    g_remainderPixels += event.preciseDeltaY * g_sensitivity;

    // Conversion to int truncates toward zero, which is exactly what an
    // accumulator needs for symmetric positive/negative motion.
    int steps = static_cast<int>(g_remainderPixels / kPixelsPerReaperStep);
    if (steps == 0) return 0.0;

    g_remainderPixels -= static_cast<double>(steps) * kPixelsPerReaperStep;
    return static_cast<double>(steps);
}

void Reset() {
    g_remainderPixels = 0.0;
    g_momentumStopped = false;
}

void SetSensitivity(double sensitivity) { g_sensitivity = sensitivity; }
double GetSensitivity() { return g_sensitivity; }

} // namespace VerticalScrollProcessor
