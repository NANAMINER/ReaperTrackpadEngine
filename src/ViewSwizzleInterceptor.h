#pragma once

// Plain C++ interface (see TrackpadInterceptor.h for why); implementation is
// Objective-C++ in ViewSwizzleInterceptor.mm.
//
// This is the "next layer down" from TrackpadInterceptor: instead of
// patching into NSApplication's event dispatch (which we've confirmed never
// sees NSEventTypeScrollWheel/NSEventTypeMagnify for REAPER's windows), this
// walks REAPER's main-window NSView tree, finds every distinct concrete
// class that defines its own -scrollWheel:/-magnifyWithEvent:, and method-
// swizzles just those two selectors on each -- logging, then always calling
// straight through to the original implementation. Pure observation: no
// REAPER behavior changes.
namespace ViewSwizzleInterceptor {

// mainHwnd is REAPER's rec->hwnd_main (an opaque HWND, i.e. void*), passed
// through by PluginEntry.cpp -- kept out of this header so it can stay plain
// C++ without depending on reaper_plugin.h's HWND type. Internally cast to
// NSView* (see TrackpadInterceptor's research notes: on macOS a SWELL HWND
// literally *is* an NSView subclass instance).
//
// Walks the view tree rooted at that NSView and swizzles
// -scrollWheel:/-magnifyWithEvent: on every distinct class found that
// overrides one of them. Must be called from the main thread, after REAPER's
// main window exists (true by the time REAPER calls a plugin's entry point).
bool Install(void *mainHwnd);

// Restores every original implementation this installed and clears
// bookkeeping. Safe to call even if not installed.
void Remove();

bool IsInstalled();

// How many distinct classes got scrollWheel:/magnifyWithEvent: swizzled.
// Zero for either is itself a useful diagnostic result (means the
// corresponding selector isn't overridden anywhere in REAPER's main view
// tree, at least not as of Install() time).
int ScrollHookCount();
int MagnifyHookCount();

} // namespace ViewSwizzleInterceptor
