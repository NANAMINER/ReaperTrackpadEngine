from pathlib import Path

root = Path('source')

# 1) Carry the actual Arrange NSView/HWND through TrackpadEvent.
p = root / 'src/TrackpadEvent.h'
s = p.read_text()
needle = '    uintptr_t receiverId = 0;\n'
insert = needle + '''\n    // Actual Arrange SWELL HWND/NSView (tag 1000), captured by the Cocoa\n    // adapter. On macOS SWELL HWNDs are NSView pointers. This is the exact\n    // window SWS GetTrackWnd()/GetArrangeWnd() ultimately resolves to.\n    uintptr_t arrangeViewId = 0;\n'''
if needle not in s:
    raise SystemExit('TrackpadEvent receiverId marker not found')
s = s.replace(needle, insert, 1)
p.write_text(s)

# 2) Store the Arrange view we already resolve from the live Cocoa hierarchy.
p = root / 'src/ViewSwizzleInterceptor.mm'
s = p.read_text()
needle = '''    NSView *arrangeAncestor = FindArrangeViewInHierarchy(view);\n    NSView *arrangeView = arrangeAncestor ?: FindViewWithTag(gMainView, kArrangeViewTag);\n    te.isInArrangeView = arrangeAncestor != nil;\n'''
replacement = '''    NSView *arrangeAncestor = FindArrangeViewInHierarchy(view);\n    NSView *arrangeView = arrangeAncestor ?: FindViewWithTag(gMainView, kArrangeViewTag);\n    te.arrangeViewId = (uintptr_t)(__bridge void *)arrangeView;\n    te.isInArrangeView = arrangeAncestor != nil;\n'''
if needle not in s:
    raise SystemExit('FillExecutionContext Arrange marker not found')
s = s.replace(needle, replacement, 1)
p.write_text(s)

# 3) v3 incorrectly attempted rec->GetFunc("GetTrackWnd"). GetTrackWnd is an\n# internal SWS helper, not a registered REAPER API. Use the real Arrange view\n# captured above instead. CF_GetScrollInfo/CF_SetTcpScroll remain dynamically\n# resolved public SWS APIs.
p = root / 'src/ReaperNavigation.cpp'
s = p.read_text()
old = '''    ResolveSwsScrollApi();\n    if (!g_cfGetScrollInfo || !g_cfSetTcpScroll || !g_getTrackWnd) return false;\n\n    HWND trackWnd = g_getTrackWnd();\n    if (!trackWnd) return false;\n'''
new = '''    ResolveSwsScrollApi();\n    if (!g_cfGetScrollInfo || !g_cfSetTcpScroll || event.arrangeViewId == 0)\n        return false;\n\n    HWND trackWnd = reinterpret_cast<HWND>(event.arrangeViewId);\n'''
if old not in s:
    raise SystemExit('v3 GetTrackWnd vertical block not found')
s = s.replace(old, new, 1)
p.write_text(s)

print('v3.1 Arrange-view vertical fix applied')
