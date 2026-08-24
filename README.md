# REAPER Trackpad Engine (macOS)

Experimental precision-trackpad input layer for REAPER on macOS. **The single
extension now contains the complete gesture engine:** smooth horizontal
Arrange scrolling, vertical Arrange/TCP scrolling with simultaneous free-2D
motion, edit-cursor-anchored pinch-to-zoom with adaptive release, and
conditioned horizontal Mixer scrolling. These behaviors have all been tested
live; the current Mixer path is the accepted best version, with a small glitch
still possible on very fast swipes.

Gesture enable states are persistent. A first installation starts safely with
the engine off; see "Enable custom behavior" below.

> **Current binary compatibility:** Apple Silicon (`arm64`) only. An Intel or
> Universal build is not included yet.

## Download

Download the ready-to-install files from
[`dist/macos-arm64`](dist/macos-arm64), then follow
[`INSTALLATION.txt`](dist/macos-arm64/INSTALLATION.txt). The binary is built
for the native Apple Silicon version of REAPER.

## Architecture

```
Cocoa / REAPER view layer  (ViewSwizzleInterceptor.mm)
        |  extracts data from a live NSEvent
        v
TrackpadEvent               (TrackpadEvent.h — pure C++, no Cocoa)
        |
        v
MotionEngine::ProcessEvent   (MotionEngine.h/.cpp — pure C++)
        |    delegates by event type to:
        |    HorizontalScrollProcessor (Scroll)  |  PinchZoomProcessor (Magnify)
        v
NavigationCommand            (NavigationCommand.h — pure C++)
        |
        v
ReaperNavigation::Apply      (ReaperNavigation.h/.cpp — only module allowed
                               to call the REAPER API)
        |
        v
GetSet_ArrangeView2()         (REAPER API — both scroll and zoom, see below)
```

Each stage has one job:
- **ViewSwizzleInterceptor** — Cocoa adapter only. Intercepts
  `-scrollWheel:`/`-magnifyWithEvent:`, extracts data (including view
  width and cursor's view-local X — retained as plain context but not
  motion data itself, see `TrackpadEvent.h`), builds a `TrackpadEvent`,
  hands it off. No smoothing, no curves, no REAPER calls. Calls REAPER's
  original implementation only when `ReaperNavigation::Apply` reports it did
  nothing.
- **TrackpadEvent** — the stable internal format. No Objective-C/AppKit/
  NSEvent/Cocoa dependency at all.
- **MotionEngine** — thin dispatcher: `Scroll` → `HorizontalScrollProcessor`,
  `Magnify` → `PinchZoomProcessor`; everything else stays zeroed. Must never
  know Cocoa or REAPER exist.
- **HorizontalScrollProcessor** / **PinchZoomProcessor** — first-version
  math for each gesture: no curve, no smoothing, no momentum modeling — just
  the trackpad's own raw delta (`preciseDeltaX` / `magnification`) ×
  a `sensitivity` multiplier (default `1.0`, no GUI/config yet — see each
  header's `SetSensitivity`/`GetSensitivity`). Output stays in the
  trackpad's own units (pixels / relative scale), never seconds — REAPER-
  specific conversion is ReaperNavigation's job, not theirs.
- **NavigationCommand** — abstract "what should happen" (scroll/zoom/track-
  height amounts), not "how". Pure data.
- **ReaperNavigation** — the *only* module allowed to touch REAPER's state.
  `Apply()` returns `true` if it actually changed REAPER's view (caller must
  suppress the native handler) or `false` if it did nothing (caller must let
  the native handler run — not over the Arrange view, missing data, sanity
  check failed, or the relevant toggle is off). The three gesture switches
  default to **disabled on first installation** and are then persistent.

Diagnostic logging (`Diagnostics.h/.cpp`, `DiagnosticEvent`) is a separate,
parallel concern — richer than `TrackpadEvent`, and deliberately NOT part of
the pipeline's stable format (see `TrackpadEvent.h`'s and `Diagnostics.h`'s
header comments for why they're split).

## How horizontal scroll actually works (`ApplyHorizontalScroll`)

1. `ViewSwizzleInterceptor` starts from the actual `NSView` that received
   `scrollWheel:` and walks up its parents. The event is accepted only if
   that hierarchy contains REAPER's Arrange view (SWELL control/tag `1000`).
   The Arrange ancestor itself supplies the width used below; other views
   bail out and retain REAPER's native handling.
2. `GetSet_ArrangeView2(NULL, false, 0, 0, &start, &end)` — read the
   currently visible time range.
3. `pixelsPerSecond = viewWidthPixels / (end - start)`;
   `deltaSeconds = -deltaPixels / pixelsPerSecond` (negated — see below).
4. Shift both bounds by `deltaSeconds`, write back with
   `GetSet_ArrangeView2(NULL, true, 0, 0, &newStart, &newEnd)`.
5. `UpdateTimeline()` to redraw.
6. Only if all of the above ran: report "handled" so
   `ViewSwizzleInterceptor` suppresses REAPER's native `scrollWheel:` for
   this event (no double scroll). Any early bail-out means REAPER's native
   handling runs completely unmodified for that event.

**Tested (single-screen setup):** engaged correctly over the Arrange view,
direction was inverted (now fixed — negated in `ApplyHorizontalScroll` so a
rightward swipe follows native REAPER's direction), felt noticeably smoother
than native REAPER scrolling even with zero curve/smoothing math applied —
i.e. just routing around whatever REAPER's native `scrollWheel:` path does
internally already helps. That's a meaningful data point for the project's
original hypothesis. The original coordinate-based gate later proved fragile
after a window-layout change; both Cocoa-derived coordinates and REAPER's own
`GetMousePosition` → `GetThingFromPoint` pair subsequently failed live. The
current implementation avoids hit-testing entirely and identifies Arrange
from the receiving NSView hierarchy/tag. Live session `20260823-203957`
confirmed the separation: custom horizontal scroll was applied only while
the target was Arrange, with native fallback preserved in the mixer.
Whether `UpdateTimeline()` is redundant with
`GetSet_ArrangeView2`'s own redraw remains unverified (harmless if so).

## How pinch-to-zoom actually works (`ApplyZoom`)

Same context-check and `GetSet_ArrangeView2` read/write as horizontal
scroll, but computes an anchor so REAPER's edit cursor stays fixed:

1. Context check + read `start`/`end`, same as scroll.
2. Read the edit-cursor time with `GetCursorPositionEx(nullptr)` and compute
   its fractional position in the visible interval (clamped to the nearest
   edge if it is off-screen).
3. `scaleFactor = 1.0 + zoomAmount` (Apple's own convention for
   `NSEvent.magnification`).
4. `newDuration = (end - start) / scaleFactor`.
5. `anchorTime = start + fraction * (end - start)`;
   `newStart = anchorTime - fraction * newDuration`; `newEnd = newStart +
   newDuration` — algebraically, the edit cursor's time stays at the same
   fractional position in the view before and after.
6. Write back, `UpdateTimeline()`, report "handled" exactly like scroll.

No separate zoom API was needed — `GetSet_ArrangeView2` (already validated
by horizontal scroll) does both jobs, just fed different math. `adjustZoom()`
exists in the SDK too but its `centermode` parameter isn't documented well
enough to guarantee edit-cursor anchoring, so this computes the anchor directly
instead of trusting it.

**Direction is confirmed**: pinch-out (spread fingers, positive
`magnification`) zooms IN (shows less time). No explicit clamp against
REAPER's own min/max zoom level exists (it relies on
`GetSet_ArrangeView2`'s internal validation), so an extreme pinch remains a
useful edge-case test.

**Tested (raw, pre-momentum):** engaged, but felt "tremendo" (harsh) — no
smoothing/curve, and it stopped dead the instant the fingers stopped, unlike
scroll which felt smooth even unsmoothed. Root cause, confirmed against
Apple's own docs: `NSEventTypeScrollWheel` has `momentumPhase` (macOS keeps
generating decaying events after the fingers lift); `NSEventTypeMagnify`
has no equivalent at all. That's a platform gesture-model difference, not
something wrong in this code -- see `PinchZoomProcessor.h`'s header comment.

## Synthesized pinch-zoom momentum (`PinchZoomProcessor`)

Since macOS doesn't provide inertia for pinch gestures, `PinchZoomProcessor`
now synthesizes it, per the model from the original project brief:

- **While the gesture is active** (`Process()`, called per real `Magnify`
  event): tracks `velocity = zoomAmount / dt` between consecutive events
  (`dt` from `NSEvent.timestamp`, same clock the events are already on).
- **On release** (`PhaseEnded`): the terminal event is applied but does not
  overwrite the last active-event velocity. A smooth velocity curve seeds
  almost no momentum below `0.75` scale units/s, grows progressively through
  medium gestures, and reaches a maximum 40% seed at `8.0` units/s. This
  gives slow pinches less release and fast pinches more without a hard
  threshold. Gestures that are both fast and shorter than 150 ms receive an
  additional smooth boost, up to 12% at 60 ms or less. `PhaseCancelled`
  stops without momentum. The last Arrange-view context is cached because no
  live `NSEvent` exists after release.
- **Every REAPER timer tick** (`Tick()`, driven by `MotionEngine::Tick()` /
  `PluginEntry.cpp`'s `TimerTick`, real elapsed time via
  `std::chrono::steady_clock` — unrelated to `NSEvent.timestamp`, momentum
  has no live event to read a timestamp from): `velocity *= exp(-decay*dt)`;
  applies `velocity*dt` as this tick's zoom delta through the exact same
  `ReaperNavigation::Apply(event, command)` a real event would use, reusing
  the cached context. Stops once `|velocity| < stopThreshold`.
- **New input cancels old momentum**: if a fresh gesture starts while
  momentum from a previous one is still decaying, `Process()` immediately
  deactivates it — no fighting between synthesized and real input.

No new thread is needed — this reuses the timer registration
that already existed for `Diagnostics::Flush()`.

**`decayPerSecond` is live-tuned to `5.0`; `stopThreshold` remains `0.02` and
may still need final feel tuning.** Higher decay = shorter,
snappier momentum; lower = longer, floatier. Change them via
`PinchZoomProcessor::SetDecay`/`SetStopThreshold` (no GUI/config yet — edit
the call site or the defaults in `PinchZoomProcessor.cpp` and rebuild for
now). The diagnostic log's `momentum` lines (see below) show the actual
`velocity`/`dt`/`amount` numbers each tick, which is what tuning these two
against should be based on, not guessing further.

## Vertical scroll — calibrated first implementation

Unlike horizontal scroll, there is **no documented REAPER API to read or set
the Arrange/TCP view's vertical scroll position**. Checked exhaustively
against the vendored SDK header:
- `GetMixerScroll`/`SetMixerScroll` exist but control the **Mixer** panel,
  not the Arrange/TCP track list.
- `I_TCPY` (per-track Y position in pixels) is **read-only**.
- No "scroll to track" / "ensure visible" function exists in the core SDK.
- The only plausible lever is `CSurf_OnScroll(int xdir, int ydir)` (also
  used for horizontal scroll's cousin) — real function, but its units
  (pixels? rows? tracks?) aren't documented anywhere, including REAPER's own
  ReaScript docs.

The one-shot **"Trackpad Engine: Calibrate vertical scroll"** action measured
the primitive twice at widely separated positions: `ydir=+1` changed
first-track `I_TCPY` by exactly `-8 px`, and `-1` restored it with zero error
both times. The implementation therefore accumulates precise trackpad Y
pixels and emits one integer REAPER unit per 8 pixels, preserving fractional
remainders. `ScrollGestureProcessor` classifies every event dynamically as
horizontal, vertical, or two-axis motion. There is no gesture-level lock: the
vector may rotate continuously, so a two-finger circular motion can traverse
both axes. A component is suppressed as noise only when the other is at least
three times larger, leaving narrow dead zones near the cardinal axes and a
wide free-2D region everywhere else.

The first live test established that the direct sign was reversed, so the
adapter now maps positive `preciseDeltaY` to negative `ydir`. It also consumes
every event routed vertically, including events that only
add a sub-8-pixel remainder; otherwise those zero-output events fall through
to REAPER's native scroll and alternate native/custom motion at release. The
final native-momentum tail is cut once its Y delta reaches 1 px/event, because
REAPER's 8 px primitive cannot render that last tail without an isolated tick.

## Status

- [x] Loads as a REAPER extension.
- [x] Two independent, parallel capture layers, both observation-only (see
      "Debug log" below for why there are two):
      1. **AppMonitor** — `[NSEvent addLocalMonitorForEventsMatchingMask:]`,
         scoped to REAPER's own process (`TrackpadInterceptor.mm`).
      2. **ViewSwizzle** — method swizzle on `-scrollWheel:`/
         `-magnifyWithEvent:` for every distinct class in REAPER's main
         window's view tree that overrides one of them
         (`ViewSwizzleInterceptor.mm`).
- [x] Logs every captured event (timestamp, deltas, precise deltas, phase,
      momentum phase, magnification, modifiers, window/view, which layer/
      class caught it) to a batched diagnostic log file.
- [x] Every scroll/magnify event flows through
      `TrackpadEvent → MotionEngine::ProcessEvent → NavigationCommand →
      ReaperNavigation::Apply`, logged as `[Trackpad]`/`[MotionEngine]`/
      `[Navigation]` trace lines.
- [x] **Custom horizontal Arrange-view scrolling** — implemented, tested,
      direction fixed. Replaces REAPER's native `scrollWheel:` handling when
      active (see "How horizontal scroll actually works" above). Off by
      default.
- [x] **Custom pinch-to-zoom, edit-cursor-anchored** — implemented. Direction
      fluidity, edit-cursor anchor and final release tuning are confirmed.
      Replaces REAPER's native `magnifyWithEvent:` handling when active (see
      "How pinch-to-zoom actually works" above). Off by default.
- [x] **Synthesized momentum for pinch-zoom** — implemented and live-tested
      as fluid. Its release follows a smooth velocity curve (less for slow
      gestures, more for fast gestures, with a short-fast boost). macOS
      provides no native momentum for magnify gestures
      (unlike scroll), so `PinchZoomProcessor` synthesizes decaying velocity
      itself, driven by the existing REAPER timer callback — see
      "Synthesized pinch-zoom momentum" above.
- [x] **Custom vertical Arrange/TCP scrolling** — implemented from the
      calibrated 8 px/unit primitive, with remainder accumulation, native
      momentum relay, dynamic free-2D event routing,
      continuous native-handler suppression, a 1 px momentum-tail cutoff,
      corrected direction, and an independent toggle. Linear, diagonal and
      continuous circular motion are confirmed.
- [x] **Persistent gesture enable state and master action** — implemented via
      REAPER ExtState. One action enables/disables the whole engine; the three
      individual gesture actions remain available as advanced controls.
- [x] **TCP and Mixer surface targeting** — receivers are learned from their
      actual effect on REAPER's TCP/MCP state. TCP vertical scrolling is
      confirmed smooth. Mixer horizontal scrolling uses the accepted
      fractional-delta conditioning path; fast swipes can still show a small
      glitch.
- [ ] Track height and GUI — **not started**. Motion tuning parameters are
      still hardcoded.
- [ ] Response curve / smoothing for horizontal scroll — **not started**,
      still raw delta × `sensitivity`. Showed a slight stutter right at the
      tail of momentum deceleration during testing — suspected to originate
      in macOS's own momentum event stream (irregular near the end) rather
      than in this extension, but not yet confirmed; revisit once zoom
      momentum has also been tested live.

### Debug log

1. AppMonitor installed, mask temporarily widened to `NSEventMaskAny`:
   confirmed it fires reliably for mouse/keyboard events but **never** for
   `NSEventTypeScrollWheel`/`NSEventTypeMagnify`, even though those gestures
   visibly work in REAPER's own UI. Conclusion: REAPER/SWELL delivers
   trackpad gesture events through a path that doesn't go through
   `-[NSApplication sendEvent:]` (what local monitors patch into), so this
   layer structurally cannot see them. Mask narrowed back down to
   `NSEventMaskScrollWheel | NSEventMaskMagnify` (see `TrackpadInterceptor.mm`).
2. **Current step**: added the ViewSwizzle layer to check the next place
   those events could be dispatched — directly on whatever `NSView` subclass
   REAPER's main window tree actually uses. At extension load, the console
   now also reports `scrollWheel: hooked on N class(es)` — if `N` is 0, no
   class in the main-window tree overrides `scrollWheel:` at all, which
   would point further still (e.g. a different `NSWindow` entirely, or
   REAPER intercepting even more centrally). Log lines from this layer carry
   `layer=viewswizzle class=<RealRuntimeClassName>`, which is itself useful
   information about REAPER's internals regardless of the outcome.

## Directory layout

```
ReaperTrackpadEngine/
  CMakeLists.txt
  src/
    PluginEntry.cpp                REAPER extension entry point, action/timer registration
    TrackpadEvent.h                 Pure C++ pipeline event format (no Cocoa dependency)
    NavigationCommand.h             Pure C++ pipeline output format (no Cocoa/REAPER dependency)
    MotionEngine.h/.cpp             Pipeline stage: dispatches by event type; Tick() drives momentum
    HorizontalScrollProcessor.h/.cpp  First-version horizontal scroll math (raw delta x sensitivity)
    PinchZoomProcessor.h/.cpp       Pinch zoom math + synthesized momentum (velocity decay)
    ReaperNavigation.h/.cpp         Pipeline stage: applies NavigationCommand via GetSet_ArrangeView2
    CocoaEventUtil.h/.mm            Shared NSEvent -> DiagnosticEvent field extraction (Objective-C++)
    TrackpadInterceptor.h/.mm       Capture layer 1: NSApplication local event monitor (diagnostic only)
    ViewSwizzleInterceptor.h/.mm    Capture layer 2: scrollWheel:/magnifyWithEvent: swizzle; feeds the pipeline
    Diagnostics.h/.cpp              DiagnosticEvent, ring buffer, batched log file writer, pipeline Trace()
  scripts/
    install.sh                Copies the built dylib into REAPER's UserPlugins
  third_party/
    reaper-sdk/sdk/            Vendored reaper_plugin.h, reaper_plugin_functions.h
    reaper-sdk/WDL/swell/      Vendored swell.h/swell-types.h/swell-functions.h
    NOTICE.md                  Provenance/license notes for the above
```

## Build

Requires Xcode command line tools and CMake (both already present on this
machine: Apple clang 16, cmake 4.2.3).

```bash
cd ReaperTrackpadEngine
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Produces `build/reaper_trackpadengine.dylib` (arm64, matching this Mac).

## Install

```bash
./scripts/install.sh
```

This copies the dylib to
`~/Library/Application Support/REAPER/UserPlugins/reaper_trackpadengine.dylib`.
REAPER only auto-loads UserPlugins files whose name starts with `reaper_`,
which is why the CMake target is named that way.

**You must fully quit and restart REAPER** for it to pick up a new or
updated dylib — REAPER does not hot-reload extensions.

## Verify it loaded

On successful load, the extension immediately calls `ShowConsoleMsg`, which
makes REAPER open its floating "console output" window (the same one
ReaScript/EEL print to) and print something like:

```
[TrackpadEngine] loaded (REAPER 7.79/arm64). log file=ok
[TrackpadEngine] app monitor=installed. view swizzle=installed (scrollWheel: hooked on N class(es), magnifyWithEvent: hooked on M class(es))
[TrackpadEngine] complete gesture engine: DISABLED/PARTIAL (persistent).
[TrackpadEngine] horizontal/vertical/pinch: OFF / OFF / OFF.
[TrackpadEngine] main action: "Trackpad Engine: Toggle engine (all gestures)". Advanced actions remain available.
```

The `N`/`M` counts are worth reading even before doing any gesture testing:
`0` for either means no class in REAPER's main-window view tree overrides
that selector at all, which is itself a significant finding (see "Debug
log" above).

If that window doesn't appear (it can end up behind the main window), the
more reliable check is the Action list: open it (`?` menu → Show action
list, or the default shortcut) and search for **"Trackpad Engine"**. The
engine and advanced actions should show up if the extension loaded and
registered correctly, regardless of the console window's visibility.

## Enable custom behavior

Use this as the normal workflow (Action list, search "Trackpad Engine"):

- **"Trackpad Engine: Toggle engine (all gestures)"** — the main master
  switch. It enables or disables horizontal scrolling (Arrange and Mixer),
  vertical scrolling (Arrange and TCP), and pinch zoom together. Its checked
  state means all three gesture switches are on.

The state of all three gesture switches is saved in REAPER ExtState and
restored at the next launch. On a first installation they are all off; run the
master action once to enable the complete engine. The following actions remain
available for diagnostics and selective fallback:

- **"Trackpad Engine: Toggle diagnostic logging"** — controls
  `Diagnostics`. Off by default.
- **"Trackpad Engine: Toggle custom horizontal scroll"** — controls
  `ReaperNavigation`'s horizontal-scroll switch for Arrange and Mixer.
- **"Trackpad Engine: Toggle custom vertical scroll"** — controls calibrated
  Arrange/TCP vertical scrolling. Linear, diagonal and circular behavior are
  confirmed.
- **"Trackpad Engine: Toggle custom pinch zoom"** — controls
  `ReaperNavigation`'s zoom switch. Engages correctly;
  direction, fluidity, edit-cursor anchor and adaptive release are confirmed.
- **"Trackpad Engine: Calibrate vertical scroll"** — one-shot diagnostic.
  Across three timer ticks it reads first-track `I_TCPY`, calls
  `CSurf_OnScroll(0,+1)`, reads again, calls `-1` to restore, and verifies the
  result. It does not enable vertical gesture interception.

The advanced toggles can form any partial combination. In that state the master
action is unchecked; running it enables all gestures, and running it while all
three are enabled disables them all. Diagnostic logging is independent and is
not changed by the master action.

### Test procedure — horizontal scroll (done, for reference)

1. Restart REAPER after installing. Confirm the Trackpad Engine actions exist.
2. **Baseline** — everything OFF (default). Two-finger scroll horizontally
   over the Arrange view: behaves exactly as REAPER always has.
3. Turn ON logging + horizontal scroll (leave zoom off for this pass).
4. Slow scroll, fast scroll, fast swipe + release (momentum).
5. Check: no double scroll, correct direction, `context: arrange` in the log
   while over the Arrange view, unremarkable CPU.
6. Turn back OFF, confirm native behavior returns exactly to baseline.

**Result**: works, direction was inverted then fixed, felt smoother than
native even with zero smoothing. Minor residual stutter right at the tail of
momentum deceleration, not yet root-caused — see Status above.

### Test procedure — pinch zoom (partially done)

1. Everything else OFF, turn ON logging + custom pinch zoom.
2. Slow pinch (both directions: spread apart = zoom in? and pinch together =
   zoom out?), fast pinch.
3. Check:
   - **Direction** — confirmed: spreading fingers zooms in.
   - **Anchor** — does REAPER's edit cursor remain at the same on-screen X
     while zooming, or does the view drift?
   - **No double zoom**, same as the scroll check.
   - **Stability at extremes** — a fast/large pinch shouldn't collapse the
     view to nothing or invert it (guarded in code, but untested live).
4. Turn back OFF, confirm native pinch zoom returns exactly to baseline.

**Result so far**: direction and fluidity are confirmed. The requested
anchor is REAPER's edit cursor (not the mouse pointer); that revised anchor
and the reduced momentum seed await this retest.

### Test procedure — revised pinch-zoom momentum

1. Logging + custom pinch zoom ON (same as above).
2. Pinch briskly and release quickly (the motion that felt "tremendo"
   before) -- the view should now keep zooming briefly after the fingers
   lift, decaying to a stop rather than cutting off instantly.
3. Check:
   - **Feel** — does it stop noticeably more gently now? Too long/floaty,
     or still too short? This directly maps to `decayPerSecond` (higher =
     shorter) and `stopThreshold` (higher = stops sooner) in
     `PinchZoomProcessor.cpp` -- there's no config/GUI yet, so tuning means
     changing the defaults there and rebuilding.
   - **No runaway** — momentum should always come to a stop on its own
     (never drift indefinitely); starting a new pinch mid-momentum should
     cleanly take over, not fight with the decaying one.
   - **CPU during momentum** — should stay unremarkable; the timer already
     runs for `Diagnostics::Flush()`, this just adds one cheap extra branch
     per tick when momentum isn't active (the common case).
4. Pull the actual numbers for tuning:

```bash
grep 'momentum amount=' ~/Library/Application\ Support/REAPER/TrackpadEngine/logs/session-*.log | tail -40
```

Each line is one tick: `<ts> momentum amount=.. velocity=.. dt=..` -- `dt`
tells you the REAPER timer's actual tick rate during momentum (not
documented anywhere, this is how to find out), `velocity` shows the decay
curve directly, `amount` is what got applied to the Arrange view that tick.

```bash
grep '\[Trackpad\]\|\[MotionEngine\]\|\[Navigation\]\|context:' \
  ~/Library/Application\ Support/REAPER/TrackpadEngine/logs/session-*.log | tail -40
```

A successfully handled scroll event looks like:
```
... [Trackpad] received scroll event
... [MotionEngine] received TrackpadEvent
... [Navigation] received NavigationCommand
... [Navigation] context: arrange
... [Navigation] applied horizontal scroll
```
A successfully handled zoom event ends with `applied zoom` instead. A
pass-through event (not over Arrange, or the relevant switch is off) stops
after `received NavigationCommand` (or, if disabled, doesn't even reach the
context check).

## Complete kill switch

Three levels are available:

1. **Stop all gesture behavior immediately**: run "Trackpad Engine: Toggle
   engine (all gestures)" while it is checked. This disables horizontal,
   vertical and pinch behavior together and saves the off state.
2. **Stop logging only** (all capture layers stay installed, but do zero
   work per event beyond one boolean check): run "Trackpad Engine: Toggle
   diagnostic logging" again.
3. **Fully disable the extension**: quit REAPER, delete or move out
   `~/Library/Application Support/REAPER/UserPlugins/reaper_trackpadengine.dylib`,
   restart REAPER. Standard removal procedure for any REAPER extension. This
   also undoes the ViewSwizzle layer's method patching — it only exists in
   that REAPER process's memory, nothing is written to disk or persisted,
   so quitting REAPER (even a crash) fully reverts it.

With both custom-behavior switches OFF (the default), both capture layers
**always dispatch/return every event unchanged** — REAPER's own scroll/
zoom/track-height handling is completely untouched, exactly as in every
phase before this one. With a switch ON, only the matching event type
(`scrollWheel:` for horizontal scroll, `magnifyWithEvent:` for zoom) that
(a) has usable data and (b) lands on the Arrange view is affected; anything
else always passes through natively (see Architecture above).

The ViewSwizzle layer is more invasive than AppMonitor (it patches live
method implementations on REAPER's own view classes rather than just
observing dispatch), so a bug there is more likely to crash REAPER outright
rather than fail gracefully. If REAPER becomes unstable after installing
this build, use kill switch #4 immediately.

## Capture a test log

1. Make sure REAPER has been restarted since installing.
2. Run the toggle action once — console should print `... ENABLED`.
3. Perform the test gestures described in the task (two-finger scroll,
   fast scroll + release/momentum, slow pinch, fast pinch, Cmd+scroll over
   the track panel, etc.) over the Arrange view.
4. Run the toggle action again to stop (`... DISABLED`) — not required, but
   keeps the log file scoped to one session.
5. Find the log at:
   `~/Library/Application Support/REAPER/TrackpadEngine/logs/session-<timestamp>.log`

To specifically verify the Phase 2 pipeline wiring, grep for the trace lines
after a test gesture:

```bash
grep '\[Trackpad\]\|\[MotionEngine\]\|\[Navigation\]' ~/Library/Application\ Support/REAPER/TrackpadEngine/logs/session-*.log | tail -20
```

Each real scroll/magnify event should produce exactly one
`[Trackpad] received .../[MotionEngine] received TrackpadEvent/[Navigation]
received NavigationCommand` triplet, in that order, interleaved with the
`scroll`/`magnify` diagnostic lines from the same event.

To watch it live while testing, in a separate terminal:

```bash
tail -f ~/Library/Application\ Support/REAPER/TrackpadEngine/logs/session-*.log
```

### Log line format

```
<ts> scroll   dx=.. dy=.. pdx=.. pdy=.. precise=0/1 phase=.. momentum=.. mods=.. rawmods=.. dt=.. layer=.. class=.. win=.. view=.. loc=(x,y)
<ts> magnify  mag=..                                phase=..              mods=.. rawmods=.. dt=.. layer=.. class=.. win=.. view=.. loc=(x,y)
<ts> other    type=N(Name)                                                mods=..              layer=..          win=.. view=.. loc=(x,y)
<ts> momentum amount=.. velocity=.. dt=..
<ts> [Stage] message
```

The `[Stage] message` form is a pipeline trace line (`Diagnostics::Trace`),
e.g. `[Trackpad] received scroll event`, `[MotionEngine] received
TrackpadEvent`, `[Navigation] received NavigationCommand`,
`[MotionEngine] pinch momentum started`/`stopped` — see Architecture above.
The `momentum` form is one synthesized pinch-zoom momentum tick (see
"Synthesized pinch-zoom momentum" above) — `amount` is the zoom delta
applied that tick, `velocity` is the post-decay velocity, `dt` is real
elapsed seconds since the previous tick. Both `momentum` and `[Stage]`
lines' `ts` come from a separate monotonic clock (no live `NSEvent` at those
pipeline stages by design), so don't compare them numerically against
scroll/magnify `ts` values — only use ordering/dt within each stream.

`other` lines only appear if the AppMonitor mask is ever widened back to
`NSEventMaskAny` for a re-test (it's narrowed to scroll+magnify only right
now — see Status above) — `type` is the raw `NSEventType` integer plus a
name where known (`KeyDown`, `LeftMouseDown`, `MouseMoved`, ...), verified
against AppKit's real header, not guessed.

- `ts` — `NSEvent.timestamp` (seconds since system boot). Not wall-clock;
  use *differences* between consecutive lines to measure inter-event
  intervals and gesture/momentum duration.
- `dx/dy` — line-based deltas (`NSEvent.deltaX/deltaY`).
- `pdx/pdy` — precise, pixel-based deltas (`NSEvent.scrollingDeltaX/Y`),
  meaningful when `precise=1`.
- `phase` / `momentum` — `none|began|changed|ended|cancelled|stationary|maybegin`,
  mirroring `NSEvent.phase` / `NSEvent.momentumPhase`.
- `mods` — held modifiers (`cmd`, `opt`, `ctrl`, `shift`, combined with `+`,
  or `none`). Use this to isolate the Cmd+scroll track-height gesture in the
  log.
- `rawmods` — `NSEvent.modifierFlags`, the complete, unfiltered bitmask (hex)
  — includes bits `mods` doesn't decode (Caps Lock, Fn, numeric pad, device-
  independent flags, ...).
- `dt` — seconds since the previous logged line of the *same event type and
  same layer* (scroll and magnify tracked independently); `n/a` for the
  first one in the session. This is the field to look at for arrival
  frequency/regularity and momentum behavior (FASE 1 goals below).
- `layer` — `appmonitor` or `viewswizzle`, which capture mechanism produced
  this line (see Status above).
- `class` — (viewswizzle only) the real Objective-C runtime class name of
  the view `-scrollWheel:`/`-magnifyWithEvent:` was called on. `-` for
  appmonitor lines.
- `win` — `NSWindow.windowNumber` under the cursor.
- `view` — opaque pointer identity (hex) of the relevant `NSView`; not a
  meaningful address on its own, just useful to see when repeated events
  share the same view vs. move to a different one. Meaning differs by
  layer: for `appmonitor` it's a hit-test guess (whatever view is under the
  cursor); for `viewswizzle` it's the actual object the event was sent to
  (`self` inside the swizzled method — ground truth, not a guess).

## Uninstall / rebuild loop while iterating

```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./scripts/install.sh
# quit REAPER fully, then reopen
```

## License

ReaperTrackpadEngine is released under the [MIT License](LICENSE). Vendored
REAPER SDK and WDL/SWELL headers retain their original zlib-style notices; see
[`third_party/NOTICE.md`](third_party/NOTICE.md).
