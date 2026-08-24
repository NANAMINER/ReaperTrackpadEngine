#import "CocoaEventUtil.h"

#include "TrackpadInterceptor.h"
#include "Diagnostics.h"

namespace {

using Diagnostics::DiagnosticEvent;
using Diagnostics::DiagnosticEventType;
using Diagnostics::CaptureLayer;

id gMonitor = nil; // __strong under ARC (see CMakeLists.txt: this file is built with -fobjc-arc)

void FillCommon(DiagnosticEvent &out, NSEvent *e) {
    FillCommonEventFields(out, e);
    out.captureLayer = CaptureLayer::AppMonitor;

    // Cheap: just record the pointer identity of whatever view is under the
    // cursor. Never dereferenced/messaged outside this file, so this can't
    // outlive the view (we only ever print it as a hex number).
    NSWindow *w = e.window;
    NSPoint loc = e.locationInWindow;
    NSView *hit = w ? [w.contentView hitTest:loc] : nil;
    out.hitViewId = (uintptr_t)hit;
}

NSEvent *HandleEvent(NSEvent *event) {
    switch (event.type) {
        case NSEventTypeScrollWheel: {
            DiagnosticEvent de;
            de.type = DiagnosticEventType::Scroll;
            FillCommon(de, event);
            de.deltaX = event.deltaX;
            de.deltaY = event.deltaY;
            de.preciseDeltaX = event.scrollingDeltaX;
            de.preciseDeltaY = event.scrollingDeltaY;
            de.hasPreciseDeltas = event.hasPreciseScrollingDeltas;
            de.phase = (uint8_t)event.phase;
            de.momentumPhase = (uint8_t)event.momentumPhase;
            Diagnostics::Capture(de);
            break;
        }
        case NSEventTypeMagnify: {
            DiagnosticEvent de;
            de.type = DiagnosticEventType::Magnify;
            FillCommon(de, event);
            de.magnification = event.magnification;
            de.phase = (uint8_t)event.phase;
            Diagnostics::Capture(de);
            break;
        }
        default: {
            // DIAGNOSTIC: only reachable if the mask below is ever widened
            // again (e.g. back to NSEventMaskAny, as it briefly was to
            // confirm the monitor fires for mouse/keyboard but never for
            // scroll/magnify -- see README "Status"). Left in place since
            // it's free and documents that capability.
            DiagnosticEvent de;
            de.type = DiagnosticEventType::Other;
            FillCommon(de, event);
            de.rawEventType = (uint32_t)event.type;
            Diagnostics::Capture(de);
            break;
        }
    }
    // Observation-only: always pass the event through untouched, regardless
    // of type. This does not change what REAPER does with any event.
    return event;
}

} // namespace

namespace TrackpadInterceptor {

bool Install() {
    if (gMonitor) return true;

    // Confirmed (see README): this monitor reliably fires for mouse and
    // keyboard events but NEVER for NSEventTypeScrollWheel/NSEventTypeMagnify
    // on REAPER's windows, even though those gestures visibly work in
    // REAPER's own UI. That means REAPER/SWELL delivers trackpad gesture
    // events through a path that doesn't go through -[NSApplication
    // sendEvent:], which is what addLocalMonitorForEventsMatchingMask:
    // patches into -- so this layer structurally cannot see them, regardless
    // of mask. Narrowed back down (from a temporary NSEventMaskAny) now that
    // the question is answered; ViewSwizzleInterceptor is the follow-up.
    NSEventMask mask = NSEventMaskScrollWheel | NSEventMaskMagnify;
    gMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                       handler:^NSEvent *(NSEvent *event) {
        return HandleEvent(event);
    }];
    return gMonitor != nil;
}

void Remove() {
    if (gMonitor) {
        [NSEvent removeMonitor:gMonitor];
        gMonitor = nil;
    }
}

bool IsInstalled() { return gMonitor != nil; }

} // namespace TrackpadInterceptor
