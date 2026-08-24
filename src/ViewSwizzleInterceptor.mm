#import "CocoaEventUtil.h"
#import <objc/runtime.h>

#include "ViewSwizzleInterceptor.h"
#include "TrackpadEvent.h"
#include "Diagnostics.h"
#include "MotionEngine.h"
#include "NavigationCommand.h"
#include "ReaperNavigation.h"

#include <vector>
#include <cstring>
#include <cmath>

namespace {

using Diagnostics::DiagnosticEvent;
using Diagnostics::DiagnosticEventType;
using Diagnostics::CaptureLayer;

struct SwizzleRecord {
    Class cls;
    SEL sel;
    IMP originalImp;
};

std::vector<SwizzleRecord> gRecords;
bool gInstalled = false;
NSView *gMainView = nil;
uintptr_t gLoggedConditionedMixerReceiverId = 0;
constexpr double kMixerPixelsPerLineUnit = 15.0;

// REAPER's main Arrange view has SWELL dialog-control ID 1000. On macOS
// SWELL represents HWNDs as NSViews and control IDs as NSView tags. Walking
// upward from the actual scrollWheel:/magnifyWithEvent: receiver also covers
// any child view REAPER may place inside the Arrange view.
constexpr NSInteger kArrangeViewTag = 1000;
constexpr int kMaxAncestorDepth = 16;
constexpr int kMaxDescendantDepth = 5;
constexpr int kMaxDescendantNodes = 64;

NSView *FindArrangeViewInHierarchy(NSView *view) {
    for (NSView *candidate = view; candidate; candidate = candidate.superview) {
        if (candidate.tag == kArrangeViewTag) return candidate;
    }
    return nil;
}

NSView *FindViewWithTag(NSView *view, NSInteger tag, int depth = 0) {
    if (!view || depth > 64) return nil;
    if (view.tag == tag) return view;
    for (NSView *child in view.subviews) {
        if (NSView *match = FindViewWithTag(child, tag, depth + 1)) return match;
    }
    return nil;
}

bool IsTrackControlPanelReceiver(NSView *receiver, NSView *arrangeView,
                                 NSEvent *event) {
    if (!receiver || !arrangeView || !event) return false;

    // TCP and Arrange are separate sibling surfaces. Require their window-
    // space edges to touch and the event to be in the vertical overlap; this
    // excludes unrelated views living in a left-side docker.
    NSRect receiverRect = [receiver convertRect:receiver.bounds toView:nil];
    NSRect arrangeRect = [arrangeView convertRect:arrangeView.bounds toView:nil];
    NSPoint point = event.locationInWindow;
    constexpr CGFloat kEdgeTolerance = 4.0;

    CGFloat edgeGap = NSMinX(arrangeRect) - NSMaxX(receiverRect);
    NSRect verticalOverlap = NSIntersectionRect(receiverRect, arrangeRect);
    return std::abs(edgeGap) <= kEdgeTolerance &&
           verticalOverlap.size.height > 0.0 &&
           point.x >= NSMinX(receiverRect) && point.x < NSMinX(arrangeRect) &&
           point.y >= NSMinY(verticalOverlap) && point.y <= NSMaxY(verticalOverlap);
}

void LogHierarchyNode(NSView *receiver, NSView *node,
                      Diagnostics::ViewHierarchyRelation relation, int depth,
                      Diagnostics::ViewSnapshotMoment moment, NSEvent *event) {
    DiagnosticEvent de;
    de.type = DiagnosticEventType::ViewHierarchy;
    de.timestampSeconds = event.timestamp;
    de.captureLayer = CaptureLayer::ViewSwizzle;
    de.hierarchyRelation = relation;
    de.hierarchyMoment = moment;
    de.hierarchyDepth = (uint8_t)depth;
    de.hierarchyTag = (int64_t)node.tag;
    de.hierarchyReceiverId = (uintptr_t)(__bridge void *)receiver;
    de.hitViewId = (uintptr_t)(__bridge void *)node;
    de.hierarchyParentId = (uintptr_t)(__bridge void *)node.superview;
    de.hierarchyEnclosingScrollViewId =
        (uintptr_t)(__bridge void *)node.enclosingScrollView;

    NSRect frame = node.frame;
    NSRect bounds = node.bounds;
    de.hierarchyFrameX = frame.origin.x;
    de.hierarchyFrameY = frame.origin.y;
    de.hierarchyFrameWidth = frame.size.width;
    de.hierarchyFrameHeight = frame.size.height;
    de.hierarchyBoundsX = bounds.origin.x;
    de.hierarchyBoundsY = bounds.origin.y;
    de.hierarchyBoundsWidth = bounds.size.width;
    de.hierarchyBoundsHeight = bounds.size.height;
    de.hierarchyIsFlipped = node.isFlipped;
    de.hierarchyIsHidden = node.isHidden;
    de.hierarchyIsScrollView = [node isKindOfClass:[NSScrollView class]];
    de.hierarchyIsClipView = [node isKindOfClass:[NSClipView class]];
    de.windowNumber = node.window ? (int32_t)node.window.windowNumber : 0;
    strlcpy(de.viewClassName, class_getName(object_getClass(node)),
            sizeof(de.viewClassName));
    Diagnostics::Capture(de);
}

void LogDescendants(NSView *receiver, NSView *node, int depth, int &remaining,
                    Diagnostics::ViewSnapshotMoment moment, NSEvent *event) {
    if (depth > kMaxDescendantDepth || remaining <= 0) return;
    for (NSView *child in node.subviews) {
        if (remaining <= 0) return;
        LogHierarchyNode(receiver, child,
                         Diagnostics::ViewHierarchyRelation::Descendant,
                         depth, moment, event);
        --remaining;
        LogDescendants(receiver, child, depth + 1, remaining, moment, event);
    }
}

void LogViewHierarchy(NSView *receiver, Diagnostics::ViewSnapshotMoment moment,
                      NSEvent *event) {
    if (!Diagnostics::IsEnabled() || !receiver) return;

    LogHierarchyNode(receiver, receiver,
                     Diagnostics::ViewHierarchyRelation::Receiver,
                     0, moment, event);

    int depth = 1;
    for (NSView *ancestor = receiver.superview;
         ancestor && depth <= kMaxAncestorDepth;
         ancestor = ancestor.superview, ++depth) {
        LogHierarchyNode(receiver, ancestor,
                         Diagnostics::ViewHierarchyRelation::Ancestor,
                         depth, moment, event);
    }

    int remaining = kMaxDescendantNodes;
    LogDescendants(receiver, receiver, 1, remaining, moment, event);
}

// ---- Diagnostic capture (unchanged from Phase 1, just renamed types) ----

void LogViewLevelEvent(id selfObj, Class cls, TrackpadEventType type, NSEvent *event) {
    DiagnosticEvent de;
    de.type = (type == TrackpadEventType::Scroll) ? DiagnosticEventType::Scroll
                                                    : DiagnosticEventType::Magnify;
    FillCommonEventFields(de, event);
    de.captureLayer = CaptureLayer::ViewSwizzle;
    // Ground truth, unlike the AppMonitor layer's hit-test guess: this *is*
    // the object -scrollWheel:/-magnifyWithEvent: was sent to.
    de.hitViewId = (uintptr_t)(__bridge void *)selfObj;
    strlcpy(de.viewClassName, class_getName(cls), sizeof(de.viewClassName));

    if (type == TrackpadEventType::Scroll) {
        de.deltaX = event.deltaX;
        de.deltaY = event.deltaY;
        de.preciseDeltaX = event.scrollingDeltaX;
        de.preciseDeltaY = event.scrollingDeltaY;
        de.hasPreciseDeltas = event.hasPreciseScrollingDeltas;
        de.phase = (uint8_t)event.phase;
        de.momentumPhase = (uint8_t)event.momentumPhase;
    } else { // Magnify
        de.magnification = event.magnification;
        de.phase = (uint8_t)event.phase;
    }
    Diagnostics::Capture(de);
}

// ---- Motion pipeline adapter (new in Phase 2) ----
//
// This is ViewSwizzleInterceptor's entire responsibility toward the motion
// pipeline: extract data from the live NSEvent, build the internal
// TrackpadEvent, hand it downstream. No curves, no smoothing happen here --
// see MotionEngine.cpp. REAPER API calls happen only in ReaperNavigation.cpp.

// Fills the execution-context fields that ReaperNavigation's scroll/zoom math
// needs but the motion processors themselves don't. The Arrange decision is
// derived from the actual receiving view hierarchy, with no screen-coordinate
// conversion or hit test.
void FillExecutionContext(TrackpadEvent &te, NSEvent *event, NSView *view) {
    te.receiverId = (uintptr_t)(__bridge void *)view;
    NSView *arrangeAncestor = FindArrangeViewInHierarchy(view);
    NSView *arrangeView = arrangeAncestor ?: FindViewWithTag(gMainView, kArrangeViewTag);
    te.isInArrangeView = arrangeAncestor != nil;
    te.isInTrackControlPanel = !te.isInArrangeView &&
        (IsTrackControlPanelReceiver(view, arrangeView, event) ||
         ReaperNavigation::IsKnownTrackControlPanel(te.receiverId));

    // Use the Arrange ancestor itself for geometry even if a child view was
    // the event receiver. Non-Arrange values are immaterial because
    // ReaperNavigation will reject that event before using them.
    NSView *geometryView = arrangeView ?: view;
    te.viewWidthPixels = geometryView.bounds.size.width;

    // View-local X, for pinch-zoom's cursor anchor (ApplyZoom in
    // ReaperNavigation.cpp) -- plain Cocoa geometry, no REAPER API involved.
    NSPoint localPoint = [geometryView convertPoint:event.locationInWindow fromView:nil];
    te.localX = localPoint.x;
}

TrackpadEvent BuildPipelineEvent(TrackpadEventType type, NSEvent *event, NSView *view) {
    TrackpadEvent te;
    te.type = type;
    te.timestamp = event.timestamp;
    te.modifiers = ModifiersFromEvent(event);
    FillExecutionContext(te, event, view);

    if (type == TrackpadEventType::Scroll) {
        te.deltaX = event.deltaX;
        te.deltaY = event.deltaY;
        te.preciseDeltaX = event.scrollingDeltaX;
        te.preciseDeltaY = event.scrollingDeltaY;
        te.hasPreciseDeltas = event.hasPreciseScrollingDeltas;
        te.phase = (uint8_t)event.phase;
        te.momentumPhase = (uint8_t)event.momentumPhase;
    } else { // Magnify
        te.magnification = event.magnification;
        te.phase = (uint8_t)event.phase;
    }
    return te;
}

// Returns true if ReaperNavigation actually changed REAPER's state for this
// event -- the caller must then suppress the original scrollWheel:/
// magnifyWithEvent: implementation (no double motion). False means "did
// nothing, let the original implementation run".
struct PipelineResult {
    TrackpadEvent event;
    bool handled = false;
};

PipelineResult RunPipeline(TrackpadEventType type, NSEvent *event, NSView *view) {
    Diagnostics::Trace("Trackpad",
        type == TrackpadEventType::Scroll ? "received scroll event" : "received magnify event");

    TrackpadEvent pipelineEvent = BuildPipelineEvent(type, event, view);
    Diagnostics::Trace("TrackpadTarget",
        pipelineEvent.isInArrangeView ? "view: arrange" :
        (pipelineEvent.isInTrackControlPanel ? "view: tcp" : "view: not arrange"));
    NavigationCommand command = MotionEngine::ProcessEvent(pipelineEvent);
    return {pipelineEvent, ReaperNavigation::Apply(pipelineEvent, command)};
}

NSEvent *ConditionMixerScrollEventIfNeeded(const PipelineResult &result,
                                           TrackpadEventType type,
                                           NSEvent *event) {
    if (type != TrackpadEventType::Scroll || !event ||
        !result.event.hasPreciseDeltas ||
        !ReaperNavigation::IsHorizontalScrollEnabled() ||
        !ReaperNavigation::IsKnownMixerPanel(result.event.receiverId)) {
        return event;
    }

    CGEventRef source = event.CGEvent;
    if (!source) return event;
    CGEventRef copy = CGEventCreateCopy(source);
    if (!copy) return event;

    // REAPER's MCP handler appears to use NSEvent.deltaX, whose native
    // fixed-point field is frequently zero even while scrollingDeltaX carries
    // real pixel motion. Preserve the whole event (including phases and
    // momentum), changing only that fractional horizontal line delta.
    // MCP line units are considerably more sensitive than Arrange pixels.
    // Live test at 10 px/unit felt too eager for small finger motion; 15
    // keeps the missing fractional events while reducing travel by one third.
    double fractionalDeltaX =
        result.event.preciseDeltaX / kMixerPixelsPerLineUnit;
    CGEventSetDoubleValueField(copy, kCGScrollWheelEventFixedPtDeltaAxis2,
                               fractionalDeltaX);

    NSEvent *conditioned = [NSEvent eventWithCGEvent:copy];
    CFRelease(copy);
    if (!conditioned) return event;

    if (gLoggedConditionedMixerReceiverId != result.event.receiverId) {
        gLoggedConditionedMixerReceiverId = result.event.receiverId;
        Diagnostics::Trace("MixerScroll", "conditioning fractional deltaX from precise pixels");
    }
    return conditioned;
}

// ---- Swizzling machinery (unchanged from Phase 1) ----

// Swizzles `sel` on `cls`, but only if `cls` defines it directly rather than
// just inheriting an ancestor's implementation -- class_copyMethodList
// (unlike class_getInstanceMethod) only returns methods the class itself
// implements, which is exactly the check we need: we want one hook per
// concrete override, not one per object.
//
// Always logs and runs the pipeline first; calls through to the original
// implementation ONLY when ReaperNavigation::Apply() reports it did nothing
// (see RunPipeline's return value) -- otherwise the original would produce
// REAPER's native scroll on top of ours (double motion). ReaperNavigation
// defaults to disabled (ReaperNavigation::SetEnabled), so out of the box
// this is still pass-through-only until explicitly turned on.
bool SwizzleIfOwnMethod(Class cls, SEL sel, TrackpadEventType type) {
    for (const auto &r : gRecords) {
        if (r.cls == cls && r.sel == sel) return true; // already swizzled
    }

    unsigned int count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    Method target = nullptr;
    for (unsigned int i = 0; i < count; ++i) {
        if (method_getName(methods[i]) == sel) {
            target = methods[i];
            break;
        }
    }
    free(methods);
    if (!target) return false;

    IMP originalImp = method_getImplementation(target);

    id block = ^(id selfObj, NSEvent *event) {
        LogViewLevelEvent(selfObj, cls, type, event);
        if (type == TrackpadEventType::Scroll &&
            (event.phase & NSEventPhaseBegan)) {
            LogViewHierarchy((NSView *)selfObj,
                             Diagnostics::ViewSnapshotMoment::Begin, event);
        }
        PipelineResult result = RunPipeline(type, event, (NSView *)selfObj);
        if (!result.handled) {
            ReaperNavigation::ViewportSnapshot before;
            bool observeSurface = type == TrackpadEventType::Scroll &&
                ReaperNavigation::NeedsSurfaceObservation(result.event);
            if (observeSurface) {
                before = ReaperNavigation::CaptureViewportSnapshot();
            }
            NSEvent *eventForOriginal =
                ConditionMixerScrollEventIfNeeded(result, type, event);
            ((void (*)(id, SEL, NSEvent *))originalImp)(
                selfObj, sel, eventForOriginal);
            if (observeSurface) {
                ReaperNavigation::ViewportSnapshot after =
                    ReaperNavigation::CaptureViewportSnapshot();
                ReaperNavigation::ObserveNativeScroll(result.event, before, after);
            }
        }
        if (type == TrackpadEventType::Scroll &&
            ((event.phase & (NSEventPhaseEnded | NSEventPhaseCancelled)) ||
             (event.momentumPhase & NSEventPhaseEnded))) {
            LogViewHierarchy((NSView *)selfObj,
                             Diagnostics::ViewSnapshotMoment::End, event);
        }
    };
    IMP newImp = imp_implementationWithBlock(block);
    method_setImplementation(target, newImp);

    gRecords.push_back({cls, sel, originalImp});
    return true;
}

void WalkAndSwizzle(NSView *view, NSMutableSet<NSValue *> *seenClasses, int depth,
                     int *scrollHooked, int *magnifyHooked) {
    // Depth guard only against a pathological/cyclic tree; REAPER's real UI
    // nesting is nowhere near this deep.
    if (!view || depth > 64) return;

    Class cls = object_getClass(view);
    NSValue *key = [NSValue valueWithPointer:(__bridge const void *)cls];
    if (![seenClasses containsObject:key]) {
        [seenClasses addObject:key];
        if (SwizzleIfOwnMethod(cls, @selector(scrollWheel:), TrackpadEventType::Scroll)) {
            ++*scrollHooked;
        }
        if (SwizzleIfOwnMethod(cls, @selector(magnifyWithEvent:), TrackpadEventType::Magnify)) {
            ++*magnifyHooked;
        }
    }

    for (NSView *sub in view.subviews) {
        WalkAndSwizzle(sub, seenClasses, depth + 1, scrollHooked, magnifyHooked);
    }
}

} // namespace

namespace ViewSwizzleInterceptor {

bool Install(void *mainHwnd) {
    if (gInstalled) return true;
    if (!mainHwnd) return false;

    NSView *mainView = (__bridge NSView *)mainHwnd;
    if (![mainView isKindOfClass:[NSView class]]) return false;
    gMainView = mainView;
    NSMutableSet<NSValue *> *seenClasses = [NSMutableSet set];
    int scrollHooked = 0, magnifyHooked = 0;
    WalkAndSwizzle(mainView, seenClasses, 0, &scrollHooked, &magnifyHooked);

    gInstalled = true;
    return true;
}

void Remove() {
    for (const auto &r : gRecords) {
        Method m = class_getInstanceMethod(r.cls, r.sel);
        if (m) method_setImplementation(m, r.originalImp);
    }
    gRecords.clear();
    gMainView = nil;
    gLoggedConditionedMixerReceiverId = 0;
    gInstalled = false;
}

bool IsInstalled() { return gInstalled; }

int ScrollHookCount() {
    int n = 0;
    for (const auto &r : gRecords) if (r.sel == @selector(scrollWheel:)) ++n;
    return n;
}

int MagnifyHookCount() {
    int n = 0;
    for (const auto &r : gRecords) if (r.sel == @selector(magnifyWithEvent:)) ++n;
    return n;
}

} // namespace ViewSwizzleInterceptor
