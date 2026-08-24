#pragma once

#include "TrackpadEvent.h" // reuses TrackpadPhaseBits/TrackpadModifierBits

#include <cstdint>

// Batched, allocation-free diagnostic logger.
//
// Threading model: Capture()/Trace() (called from the Cocoa capture layers)
// and Flush() (called from REAPER's registered "timer" callback) run on the
// main thread only -- that is guaranteed by the respective APIs (NSEvent
// local monitors and method-swizzled view callbacks always fire on the
// thread that installed/swizzled them, here the main thread; REAPER's
// plugin_register("timer", ...) callback is pumped from REAPER's own
// main-thread loop). Because producer and consumer can never run
// concurrently, the ring buffer below needs no locks or atomics -- see
// Diagnostics.cpp for the (debug-only) assertion that enforces this
// invariant.
namespace Diagnostics {

enum class DiagnosticEventType : uint8_t {
    Scroll = 0,
    Magnify = 1,
    // Any other NSEventType the AppMonitor local monitor observed
    // (mouse/keyboard/etc). Used to tell "monitor isn't firing at all" apart
    // from "monitor fires, but scroll/magnify specifically never arrive".
    // See TrackpadInterceptor.mm.
    Other = 2,
    // A pipeline-stage trace line (e.g. "[MotionEngine] received
    // TrackpadEvent"), added for the Phase 2 architecture split. See
    // Trace() below.
    Trace = 3,
    // One synthesized pinch-zoom momentum tick (PinchZoomProcessor::Tick).
    // Separate from Trace because tuning decay/threshold needs the actual
    // numbers (velocity, dt), not just a static message -- see
    // momentumVelocity/momentumDt below.
    Momentum = 4,
    // One completed CSurf_OnScroll vertical calibration pass. Numeric fields
    // below record the first track's TCP Y before, after +1, and after the
    // compensating -1 call.
    VerticalCalibration = 5,
    // One node from a diagnostic snapshot of the Cocoa hierarchy around the
    // real scrollWheel: receiver. Emitted only at gesture boundaries to
    // identify TCP vs Mixer containers and discover any NSScrollView/
    // NSClipView that can support pixel-precise scrolling.
    ViewHierarchy = 6,
    // One completed reversible probe of CSurf_OnScroll(xdir, 0), recording
    // both MCP X and Arrange start so the controlled surface is unambiguous.
    MixerCalibration = 7,
};

enum class ViewHierarchyRelation : uint8_t {
    Receiver = 0,
    Ancestor = 1,
    Descendant = 2,
};

enum class ViewSnapshotMoment : uint8_t {
    Begin = 0,
    End = 1,
};

// Which mechanism captured the event -- the two layers compared while
// diagnosing why scroll/magnify weren't reaching the app-level monitor.
enum class CaptureLayer : uint8_t {
    // [NSEvent addLocalMonitorForEventsMatchingMask:handler:], patched into
    // -[NSApplication sendEvent:]. See TrackpadInterceptor.mm. Confirmed
    // structurally blind to scroll/magnify on REAPER's windows.
    AppMonitor = 0,
    // Method swizzle on -scrollWheel:/-magnifyWithEvent: of whatever concrete
    // NSView subclasses REAPER's own view tree actually uses. See
    // ViewSwizzleInterceptor.mm. This is the layer that actually sees
    // trackpad gestures and feeds the motion pipeline.
    ViewSwizzle = 1,
};

// Diagnostic-only event record -- deliberately richer than TrackpadEvent.h's
// pipeline-facing TrackpadEvent (which carries only what MotionEngine needs).
// This lives here, not in TrackpadEvent.h, because it's an implementation
// detail of the diagnostics/logging concern, not part of the stable internal
// motion-pipeline format.
struct DiagnosticEvent {
    double timestampSeconds = 0.0; // NSEvent.timestamp (system uptime, seconds)
    DiagnosticEventType type = DiagnosticEventType::Scroll;

    // Scroll wheel fields (DiagnosticEventType::Scroll only)
    double deltaX = 0.0;           // NSEvent.deltaX (line-based)
    double deltaY = 0.0;           // NSEvent.deltaY (line-based)
    double preciseDeltaX = 0.0;    // NSEvent.scrollingDeltaX (pixel-based when precise)
    double preciseDeltaY = 0.0;    // NSEvent.scrollingDeltaY
    bool hasPreciseDeltas = false; // NSEvent.hasPreciseScrollingDeltas
    uint8_t momentumPhase = PhaseNone; // NSEvent.momentumPhase

    // Magnify fields (DiagnosticEventType::Magnify only). Also reused by
    // DiagnosticEventType::Momentum to hold that tick's applied zoom
    // increment (same kind of quantity: a relative scale delta).
    double magnification = 0.0;    // NSEvent.magnification (relative delta per event)

    // Momentum-only (DiagnosticEventType::Momentum) -- see
    // PinchZoomProcessor::Tick.
    double momentumVelocity = 0.0; // scale units/second, after this tick's decay
    double momentumDt = 0.0;       // seconds since the previous tick

    // VerticalCalibration-only -- see ReaperNavigation's three-tick
    // calibration state machine and PluginEntry.cpp's TimerTick.
    bool calibrationValid = false;
    double calibrationBeforeY = 0.0;
    double calibrationAfterPositiveY = 0.0;
    double calibrationAfterRestoreY = 0.0;

    // MixerCalibration-only.
    double mixerCalibrationBeforeX = 0.0;
    double mixerCalibrationAfterPositiveX = 0.0;
    double mixerCalibrationAfterRestoreX = 0.0;
    double mixerCalibrationBeforeArrange = 0.0;
    double mixerCalibrationAfterPositiveArrange = 0.0;
    double mixerCalibrationAfterRestoreArrange = 0.0;

    // ViewHierarchy-only -- see ViewSwizzleInterceptor.mm.
    ViewHierarchyRelation hierarchyRelation = ViewHierarchyRelation::Receiver;
    ViewSnapshotMoment hierarchyMoment = ViewSnapshotMoment::Begin;
    uint8_t hierarchyDepth = 0;
    int64_t hierarchyTag = 0;
    uintptr_t hierarchyReceiverId = 0;
    uintptr_t hierarchyParentId = 0;
    uintptr_t hierarchyEnclosingScrollViewId = 0;
    double hierarchyFrameX = 0.0;
    double hierarchyFrameY = 0.0;
    double hierarchyFrameWidth = 0.0;
    double hierarchyFrameHeight = 0.0;
    double hierarchyBoundsX = 0.0;
    double hierarchyBoundsY = 0.0;
    double hierarchyBoundsWidth = 0.0;
    double hierarchyBoundsHeight = 0.0;
    bool hierarchyIsFlipped = false;
    bool hierarchyIsHidden = false;
    bool hierarchyIsScrollView = false;
    bool hierarchyIsClipView = false;

    // Other fields (DiagnosticEventType::Other only)
    uint32_t rawEventType = 0;     // raw NSEvent.type value (NSEventType)

    // Common fields
    uint8_t phase = PhaseNone;     // NSEvent.phase
    uint32_t modifiers = 0;        // TrackpadModifierBits (decoded: cmd/opt/ctrl/shift only)
    uint64_t rawModifierFlags = 0; // NSEvent.modifierFlags, unmodified (full NSEventModifierFlags bitmask)

    double locationInWindowX = 0.0;
    double locationInWindowY = 0.0;
    int32_t windowNumber = 0;      // NSWindow.windowNumber, 0 if none

    CaptureLayer captureLayer = CaptureLayer::AppMonitor;

    // Opaque identity of the relevant NSView at capture time. Never
    // dereferenced outside the .mm files -- treated as a bare integer
    // everywhere else. Meaning differs by layer:
    //   AppMonitor  -> the view hit-tested under the cursor (a guess).
    //   ViewSwizzle -> `self` of the swizzled method call (ground truth --
    //                  this is literally the object the event was sent to).
    //   ViewHierarchy -> the hierarchy node described by this record; the
    //                    original receiver is hierarchyReceiverId.
    uintptr_t hitViewId = 0;

    // Populated by the ViewSwizzle layer only (object_getClass(self) name,
    // via class_getName + strlcpy -- no allocation). Empty for AppMonitor
    // events.
    char viewClassName[40] = {0};

    // Trace-only (DiagnosticEventType::Trace). Both MUST be string literals
    // or otherwise static-duration C strings -- only the pointer is stored,
    // never copied, to keep Capture()/Trace() allocation-free.
    const char *traceStage = nullptr;
    const char *traceMessage = nullptr;
};

// Creates <resourceDir>/logs/ if needed and opens a fresh timestamped log
// file inside it. resourceDir should be REAPER's resource path + "/TrackpadEngine"
// (i.e. the caller in PluginEntry.cpp builds that path using GetResourcePath()).
// Returns false if the log file could not be created.
bool Init(const char *resourceDir);

// Flushes and closes the current log file.
void Shutdown();

void SetEnabled(bool enabled);
bool IsEnabled();

// Copies the event into the ring buffer. No allocation, no I/O. Cheap no-op
// when logging is disabled. Must be called from the main thread.
void Capture(const DiagnosticEvent &event);

// Minimal pipeline-stage trace, e.g. Trace("MotionEngine", "received
// TrackpadEvent") -- used to verify the Phase 2 pipeline wiring end to end.
// stage/message MUST be string literals (or otherwise static-duration C
// strings): only the pointers are stored, not copied, so this is just as
// allocation-free as Capture() and safe to call from an event callback.
// Routed through the same ring buffer/batched flush as everything else, and
// respects the same enabled/disabled toggle.
void Trace(const char *stage, const char *message);

// Drains whatever is currently buffered and appends formatted lines to the
// log file in one batch, then flushes the C stream. Must be called from the
// main thread. Cheap no-op when the buffer is empty.
void Flush();

} // namespace Diagnostics
