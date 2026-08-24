#pragma once

// Plain C++ interface on purpose: this header is included from PluginEntry.cpp
// (compiled as ordinary C++), while the implementation (TrackpadInterceptor.mm)
// is Objective-C++. No Cocoa type ever crosses this boundary.
namespace TrackpadInterceptor {

// Installs a local NSEvent monitor (scroll wheel + magnify) scoped to this
// process (REAPER itself, since we are loaded into it). Must be called from
// the main thread, after NSApp exists (true by the time REAPER calls a
// plugin's entry point).
//
// Phase-2 behaviour: strictly observational. The handler always returns the
// event unchanged, so REAPER's own scroll/zoom handling is completely
// unaffected -- nothing is blocked, delayed, or rewritten.
bool Install();

// Removes the monitor. Safe to call even if not installed (e.g. during
// unload). Must be called before the extension's code is unmapped, or a
// later trackpad event would invoke a dangling block.
void Remove();

bool IsInstalled();

} // namespace TrackpadInterceptor
