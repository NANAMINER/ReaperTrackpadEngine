#import "CocoaEventUtil.h"

uint32_t ModifiersFromEvent(NSEvent *e) {
    NSEventModifierFlags f = e.modifierFlags;
    uint32_t m = 0;
    if (f & NSEventModifierFlagShift) m |= ModShift;
    if (f & NSEventModifierFlagControl) m |= ModControl;
    if (f & NSEventModifierFlagOption) m |= ModOption;
    if (f & NSEventModifierFlagCommand) m |= ModCommand;
    return m;
}

void FillCommonEventFields(Diagnostics::DiagnosticEvent &out, NSEvent *e) {
    out.timestampSeconds = e.timestamp;
    out.modifiers = ModifiersFromEvent(e);
    out.rawModifierFlags = (uint64_t)e.modifierFlags;

    NSPoint loc = e.locationInWindow;
    out.locationInWindowX = loc.x;
    out.locationInWindowY = loc.y;

    NSWindow *w = e.window;
    out.windowNumber = w ? (int32_t)w.windowNumber : 0;
}
