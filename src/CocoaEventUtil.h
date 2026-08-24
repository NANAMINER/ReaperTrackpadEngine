#pragma once

// Shared Cocoa-facing helpers used by both capture layers (TrackpadInterceptor.mm
// -- the NSApplication local event monitor -- and ViewSwizzleInterceptor.mm --
// the view-level scrollWheel:/magnifyWithEvent: swizzle). Objective-C++ only;
// never included from plain .cpp files.
//
// These fill Diagnostics::DiagnosticEvent (the rich, diagnostic-only record),
// not the pipeline-facing TrackpadEvent -- ViewSwizzleInterceptor.mm builds
// that one itself, separately, since it's the only capture layer that feeds
// the motion pipeline.

#import <Cocoa/Cocoa.h>
#include "Diagnostics.h"

uint32_t ModifiersFromEvent(NSEvent *e);

// Fills timestamp, modifiers (decoded + raw), location-in-window, and window
// number from the event. Does NOT touch type-specific fields (deltas/
// magnification), captureLayer, hitViewId, or viewClassName -- callers set
// those themselves since their meaning differs per capture layer.
void FillCommonEventFields(Diagnostics::DiagnosticEvent &out, NSEvent *e);
