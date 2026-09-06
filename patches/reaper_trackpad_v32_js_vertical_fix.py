from pathlib import Path

root = Path('source')
p = root / 'src/ReaperNavigation.cpp'
s = p.read_text()

old = r'''using ReaperGetFunc = void *(*)(const char *);
using CFGetScrollInfoFn = int (*)(HWND, int, SCROLLINFO *);
using CFSetTcpScrollFn = void (*)(MediaTrack *, int);
using GetTrackWndFn = HWND (*)();

ReaperGetFunc g_reaperGetFunc = nullptr;
CFGetScrollInfoFn g_cfGetScrollInfo = nullptr;
CFSetTcpScrollFn g_cfSetTcpScroll = nullptr;
GetTrackWndFn g_getTrackWnd = nullptr;

void ResolveSwsScrollApi() {
    if (!g_reaperGetFunc) return;
    if (!g_cfGetScrollInfo) {
        g_cfGetScrollInfo = reinterpret_cast<CFGetScrollInfoFn>(
            g_reaperGetFunc("CF_GetScrollInfo"));
    }
    if (!g_cfSetTcpScroll) {
        g_cfSetTcpScroll = reinterpret_cast<CFSetTcpScrollFn>(
            g_reaperGetFunc("CF_SetTcpScroll"));
    }
    if (!g_getTrackWnd) {
        g_getTrackWnd = reinterpret_cast<GetTrackWndFn>(
            g_reaperGetFunc("GetTrackWnd"));
    }
}
'''
new = r'''using ReaperGetFunc = void *(*)(const char *);
using JSGetScrollInfoFn = bool (*)(void *, const char *, int *, int *, int *, int *, int *);
using JSSetScrollPosFn = bool (*)(void *, const char *, int);

ReaperGetFunc g_reaperGetFunc = nullptr;
JSGetScrollInfoFn g_jsGetScrollInfo = nullptr;
JSSetScrollPosFn g_jsSetScrollPos = nullptr;

void ResolvePixelScrollApi() {
    if (!g_reaperGetFunc) return;
    if (!g_jsGetScrollInfo) {
        g_jsGetScrollInfo = reinterpret_cast<JSGetScrollInfoFn>(
            g_reaperGetFunc("JS_Window_GetScrollInfo"));
    }
    if (!g_jsSetScrollPos) {
        g_jsSetScrollPos = reinterpret_cast<JSSetScrollPosFn>(
            g_reaperGetFunc("JS_Window_SetScrollPos"));
    }
}
'''
if old not in s:
    raise SystemExit('v3 API resolver block not found')
s = s.replace(old, new, 1)

old = r'''    ResolveSwsScrollApi();
    if (!g_cfGetScrollInfo || !g_cfSetTcpScroll || event.arrangeViewId == 0)
        return false;

    HWND trackWnd = reinterpret_cast<HWND>(event.arrangeViewId);

    SCROLLINFO si = { sizeof(SCROLLINFO), };
    si.fMask = SIF_ALL;
    if (!g_cfGetScrollInfo(trackWnd, SB_VERT, &si)) return false;

    long long target = static_cast<long long>(si.nPos) - pixels;
    long long minPos = si.nMin;
    long long maxPos = static_cast<long long>(si.nMax) -
                       static_cast<long long>(si.nPage) + 1;
    if (maxPos < minPos) maxPos = minPos;
    if (target < minPos) target = minPos;
    if (target > maxPos) target = maxPos;

    if (static_cast<int>(target) != si.nPos) {
        g_cfSetTcpScroll(nullptr, static_cast<int>(target));
    }
    return true;
'''
new = r'''    ResolvePixelScrollApi();
    if (!g_jsGetScrollInfo || !g_jsSetScrollPos || event.arrangeViewId == 0)
        return false;

    void *arrangeWnd = reinterpret_cast<void *>(event.arrangeViewId);
    int pos = 0;
    int page = 0;
    int minPos = 0;
    int maxPos = 0;
    int trackPos = 0;
    if (!g_jsGetScrollInfo(arrangeWnd, "v", &pos, &page,
                           &minPos, &maxPos, &trackPos)) {
        return false;
    }

    long long effectiveMax = static_cast<long long>(maxPos) -
                             static_cast<long long>(page > 0 ? page - 1 : 0);
    if (effectiveMax < minPos) effectiveMax = minPos;

    long long target = static_cast<long long>(pos) - pixels;
    if (target < minPos) target = minPos;
    if (target > effectiveMax) target = effectiveMax;

    // At a boundary we still handled the custom vertical gesture. Consuming
    // it avoids falling through to REAPER's native wheel behavior.
    if (static_cast<int>(target) == pos) return true;

    return g_jsSetScrollPos(arrangeWnd, "v", static_cast<int>(target));
'''
if old not in s:
    raise SystemExit('v3.1 vertical block not found')
s = s.replace(old, new, 1)

old = '''void SetApiResolver(void *(*getFunc)(const char *)) {
    g_reaperGetFunc = getFunc;
    ResolveSwsScrollApi();
}
'''
new = '''void SetApiResolver(void *(*getFunc)(const char *)) {
    g_reaperGetFunc = getFunc;
    ResolvePixelScrollApi();
}
'''
if old not in s:
    raise SystemExit('SetApiResolver block not found')
s = s.replace(old, new, 1)

p.write_text(s)
print('v3.2 js_ReaScriptAPI vertical fix applied')
