#pragma once

// The stable internal event format for the trackpad motion pipeline:
//
//   Cocoa / REAPER view layer (ViewSwizzleInterceptor.mm)
//         |  extracts data from a live NSEvent, builds this struct
//         v
//   TrackpadEvent                      <-- this file
//         |
//         v
//   MotionEngine::ProcessEvent(...)
//         |
//         v
//   NavigationCommand -> ReaperNavigation::Apply(...)
//
// Deliberately free of any Objective-C/AppKit/NSEvent/Cocoa dependency, so
// MotionEngine (and anything downstream) can be built and reasoned about --
// eventually unit-tested -- without linking Cocoa at all. Every field here is
// a plain value copied out of an NSEvent exactly once, by the adapter layer;
// nothing downstream ever reaches back into Cocoa.
//
// Diagnostic-only concerns (which capture layer saw the event, which NSView
// class, window number, hit-test view id, raw untyped NSEventType, ...)
// deliberately do NOT live here -- see Diagnostics.h's DiagnosticEvent for
// those. This struct carries only what the motion pipeline itself needs.

#include <cstdint>

enum class TrackpadEventType : uint8_t {
    Scroll = 0,
    Magnify = 1,
};

// Mirrors AppKit's NSEventPhase bitmask (NSEvent.h): None=0, Began=1<<0,
// Stationary=1<<1, Changed=1<<2, Ended=1<<3, Cancelled=1<<4, MayBegin=1<<5.
// Values confirmed against Apple's public NSEventPhase documentation. Kept as
// plain integer constants (not an NSEventPhase) precisely so MotionEngine can
// interpret phase/momentumPhase without including AppKit.
enum TrackpadPhaseBits : uint8_t {
    PhaseNone       = 0,
    PhaseBegan      = 1 << 0,
    PhaseStationary = 1 << 1,
    PhaseChanged    = 1 << 2,
    PhaseEnded      = 1 << 3,
    PhaseCancelled  = 1 << 4,
    PhaseMayBegin   = 1 << 5,
};

enum TrackpadModifierBits : uint32_t {
    ModShift   = 1 << 0,
    ModControl = 1 << 1,
    ModOption  = 1 << 2,
    ModCommand = 1 << 3,
};

struct TrackpadEvent {
    TrackpadEventType type = TrackpadEventType::Scroll;
    double timestamp = 0.0;        // NSEvent.timestamp (system uptime, seconds)

    // Scroll fields (type == Scroll)
    double deltaX = 0.0;           // NSEvent.deltaX (line-based)
    double deltaY = 0.0;           // NSEvent.deltaY (line-based)
    double preciseDeltaX = 0.0;    // NSEvent.scrollingDeltaX (pixel-based when precise)
    double preciseDeltaY = 0.0;    // NSEvent.scrollingDeltaY
    bool hasPreciseDeltas = false; // NSEvent.hasPreciseScrollingDeltas
    uint8_t momentumPhase = PhaseNone; // NSEvent.momentumPhase

    // Magnify fields (type == Magnify)
    double magnification = 0.0;    // NSEvent.magnification (relative delta per event)

    // Common
    uint8_t phase = PhaseNone;     // NSEvent.phase
    uint32_t modifiers = 0;        // TrackpadModifierBits (decoded: cmd/opt/ctrl/shift)

    // Execution context ReaperNavigation needs for scroll/zoom, NOT motion
    // data. Still plain values -- no Cocoa type -- so this doesn't violate
    // this struct's "no Cocoa dependency" rule.

    // Stable for the lifetime of the receiving Cocoa view. Stored as an
    // integer so downstream code can learn which SWELL surface changed TCP
    // or MCP state without importing Objective-C types.
    uintptr_t receiverId = 0;

    // True when the NSView that received the event is the Arrange view
    // (REAPER/SWELL tag 1000) or one of its descendants. Determined directly
    // from the live Cocoa view hierarchy by ViewSwizzleInterceptor, avoiding
    // coordinate-based hit testing entirely.
    bool isInArrangeView = false;

    // True when the receiver is the TCP surface immediately to the left of
    // the Arrange view. REAPER implements the TCP as a sibling SWELL view,
    // so tag 1000 is not in its ancestor chain.
    bool isInTrackControlPanel = false;

    // The receiving NSView's own width (view.frame.size.width, in points).
    // Used by ReaperNavigation to convert a pixel-space scroll delta into
    // seconds using the Arrange view's current zoom level -- see
    // ReaperNavigation.cpp. 0.0 means "unavailable"; treat as missing data.
    double viewWidthPixels = 0.0;

    // The gesture's X position within the receiving view's OWN coordinate
    // space (points, 0 = view's left edge) -- via
    // [view convertPoint:event.locationInWindow fromView:nil], plain Cocoa
    // geometry, no REAPER API involved. Retained as execution context for
    // possible future gestures; pinch zoom is anchored on REAPER's edit
    // cursor and no longer uses this value.
    double localX = 0.0;
};
