#include "PinchZoomProcessor.h"

#include "Diagnostics.h"

#include <chrono>
#include <cmath>

namespace PinchZoomProcessor {
namespace {

double g_sensitivity = 1.0;
double g_decayPerSecond = 5.0; // live-tuned: slightly longer release than the initial 6.0
double g_stopThreshold = 0.02; // first guess -- needs live tuning

// Release curve calibrated from session 20260823-210116: slow gestures ended
// around 0.8-1.3 scale units/s, medium ones around 2-5, and fast ones around
// 6-10. A fixed seed gain made the slow release too noticeable and the fast
// release too restrained. Smoothstep keeps the transition continuous at both
// ends (zero slope, so there is no perceptible threshold).
constexpr double kSlowReleaseSpeed = 0.75;
constexpr double kFastReleaseSpeed = 8.0;
constexpr double kMaxMomentumSeedGain = 0.40;
constexpr double kShortBoostStartSpeed = 5.0;
constexpr double kShortBoostFullSpeed = 8.0;
constexpr double kFullBoostDuration = 0.06;
constexpr double kNoBoostDuration = 0.15;
constexpr double kMaxShortFastBoost = 0.12;

double Smoothstep01(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return t * t * (3.0 - 2.0 * t);
}

double MomentumSeedGain(double velocity, double gestureDuration) {
    double speed = std::fabs(velocity);
    double releaseT = (speed - kSlowReleaseSpeed) /
                      (kFastReleaseSpeed - kSlowReleaseSpeed);
    double baseGain = kMaxMomentumSeedGain * Smoothstep01(releaseT);

    // A short, decisive flick gets a small extra push. Both dimensions must
    // agree: a short slow pinch and a long gesture with one fast final event
    // receive little or no boost.
    double speedT = (speed - kShortBoostStartSpeed) /
                    (kShortBoostFullSpeed - kShortBoostStartSpeed);
    double durationT = (kNoBoostDuration - gestureDuration) /
                       (kNoBoostDuration - kFullBoostDuration);
    double boost = 1.0 + kMaxShortFastBoost *
                         Smoothstep01(speedT) * Smoothstep01(durationT);

    return baseGain * boost;
}

// Velocity tracking during an active gesture. Uses NSEvent.timestamp deltas
// (via TrackpadEvent.timestamp) -- the same clock the events themselves are
// already on, consistent with how the rest of the pipeline treats it.
bool g_hasPrevEventTimestamp = false;
double g_prevEventTimestamp = 0.0;
double g_gestureStartTimestamp = 0.0;
double g_velocity = 0.0; // scale units/second

// Synthesized momentum state. Uses std::chrono::steady_clock -- deliberately
// NOT the same clock as g_prevEventTimestamp above: this only times Tick()
// calls against each other (there's no NSEvent during momentum), so a plain
// monotonic clock is all it needs, and pure C++ keeps this file Cocoa-free.
bool g_momentumActive = false;
std::chrono::steady_clock::time_point g_lastTickTime;

// Anchor/view context from the most recent real gesture event, reused by
// Tick() so momentum continues around the same point after the fingers
// lift -- there's no live NSEvent to read it from at that point.
TrackpadEvent g_cachedContext;

} // namespace

double Process(const TrackpadEvent &event) {
    if (event.type != TrackpadEventType::Magnify) return 0.0;

    double amount = event.magnification * g_sensitivity;
    g_cachedContext = event;

    const bool ended = (event.phase & PhaseEnded) != 0;
    const bool cancelled = (event.phase & PhaseCancelled) != 0;
    const bool terminal = ended || cancelled;

    // Terminal magnify events may contain zero, noise, or one last delta.
    // Apply that amount to the view, but do not let it replace the useful
    // velocity measured from the preceding active events.
    if (!terminal && g_hasPrevEventTimestamp) {
        double dt = event.timestamp - g_prevEventTimestamp;
        if (dt > 0.0) {
            g_velocity = amount / dt;
        }
    }

    if (!terminal) {
        if (!g_hasPrevEventTimestamp) {
            g_velocity = 0.0;
            g_gestureStartTimestamp = event.timestamp;
        }
        g_prevEventTimestamp = event.timestamp;
        g_hasPrevEventTimestamp = true;

        // New live input supersedes momentum left over from a previous
        // gesture.
        g_momentumActive = false;
    } else {
        // A cancelled gesture stops immediately. A normal release uses a
        // speed-dependent curve: slow pinches stop promptly while fast ones
        // get a longer, stronger tail. Even the maximum remains well below
        // the original 1.0 seed that added excessive total zoom.
        double gestureDuration = event.timestamp - g_gestureStartTimestamp;
        if (gestureDuration < 0.0) gestureDuration = 0.0;
        if (ended && g_hasPrevEventTimestamp) {
            g_velocity *= MomentumSeedGain(g_velocity, gestureDuration);
        }
        g_momentumActive = ended && g_hasPrevEventTimestamp &&
                           std::fabs(g_velocity) > g_stopThreshold;
        g_lastTickTime = std::chrono::steady_clock::now();
        g_hasPrevEventTimestamp = false; // next gesture starts fresh dt tracking
        Diagnostics::Trace("MotionEngine",
            g_momentumActive ? "pinch momentum started" : "pinch ended, no momentum (below threshold)");
    }

    return amount;
}

double Tick(TrackpadEvent &outContext) {
    if (!g_momentumActive) return 0.0;

    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - g_lastTickTime).count();
    g_lastTickTime = now;
    if (dt > 1.0) dt = 1.0; // defensive cap against an abnormally long gap between ticks

    g_velocity *= std::exp(-g_decayPerSecond * dt);

    if (std::fabs(g_velocity) < g_stopThreshold) {
        g_momentumActive = false;
        Diagnostics::Trace("MotionEngine", "pinch momentum stopped");
        return 0.0;
    }

    double amount = g_velocity * dt;

    Diagnostics::DiagnosticEvent de;
    de.type = Diagnostics::DiagnosticEventType::Momentum;
    de.timestampSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    de.magnification = amount;
    de.momentumVelocity = g_velocity;
    de.momentumDt = dt;
    Diagnostics::Capture(de);

    outContext = g_cachedContext;
    return amount;
}

bool IsMomentumActive() { return g_momentumActive; }

void SetSensitivity(double sensitivity) { g_sensitivity = sensitivity; }
double GetSensitivity() { return g_sensitivity; }
void SetDecay(double decayPerSecond) { g_decayPerSecond = decayPerSecond; }
double GetDecay() { return g_decayPerSecond; }
void SetStopThreshold(double threshold) { g_stopThreshold = threshold; }
double GetStopThreshold() { return g_stopThreshold; }

} // namespace PinchZoomProcessor
