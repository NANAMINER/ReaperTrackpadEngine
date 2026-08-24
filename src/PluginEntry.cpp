// REAPER extension entry point.
//
// Only this translation unit defines REAPERAPI_IMPLEMENT, so only this file
// actually allocates storage for the REAPER API function pointers; every
// other file gets plain `extern` declarations (see ReaperNavigation.cpp's
// top comment) -- their own REAPERAPI_WANT_* lists must be a subset of the
// ones defined here, or those externs won't resolve at link time.
//
// REAPERAPI_MINIMAL + REAPERAPI_WANT_* keeps REAPERAPI_LoadAPI() to just the
// functions this extension actually calls, instead of resolving the entire
// REAPER API surface on load.
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_ShowConsoleMsg
#define REAPERAPI_WANT_plugin_register
#define REAPERAPI_WANT_GetResourcePath
#define REAPERAPI_WANT_GetAppVersion
#define REAPERAPI_WANT_GetExtState
#define REAPERAPI_WANT_CSurf_OnScroll
#define REAPERAPI_WANT_GetMediaTrackInfo_Value
#define REAPERAPI_WANT_GetSet_ArrangeView2
#define REAPERAPI_WANT_GetCursorPositionEx
#define REAPERAPI_WANT_GetMixerScroll
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_SetExtState
#define REAPERAPI_WANT_UpdateTimeline
#define REAPERAPI_IMPLEMENT
#include "reaper_plugin_functions.h"

#include "Diagnostics.h"
#include "MotionEngine.h"
#include "NavigationCommand.h"
#include "ReaperNavigation.h"
#include "TrackpadEvent.h"
#include "TrackpadInterceptor.h"
#include "ViewSwizzleInterceptor.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// One user-facing master toggle controls the complete gesture engine. The
// existing per-feature toggles remain as advanced controls for diagnostics
// and selective fallback. Gesture states are persisted through REAPER's
// ExtState; first installation still defaults safely to all-off.

constexpr const char *kSettingsSection = "ReaperTrackpadEngine";
constexpr const char *kHorizontalSetting = "horizontal_enabled";
constexpr const char *kVerticalSetting = "vertical_enabled";
constexpr const char *kZoomSetting = "zoom_enabled";

int g_toggleMasterCommandId = 0;
gaccel_register_t g_toggleMasterGaccel{};
std::string g_toggleMasterDesc = "Trackpad Engine: Toggle engine (all gestures)";

int g_toggleLogCommandId = 0;
gaccel_register_t g_toggleLogGaccel{};
std::string g_toggleLogDesc = "Trackpad Engine: Toggle diagnostic logging";

int g_toggleScrollCommandId = 0;
gaccel_register_t g_toggleScrollGaccel{};
std::string g_toggleScrollDesc = "Trackpad Engine: Toggle custom horizontal scroll";

int g_toggleVerticalCommandId = 0;
gaccel_register_t g_toggleVerticalGaccel{};
std::string g_toggleVerticalDesc = "Trackpad Engine: Toggle custom vertical scroll";

int g_toggleZoomCommandId = 0;
gaccel_register_t g_toggleZoomGaccel{};
std::string g_toggleZoomDesc = "Trackpad Engine: Toggle custom pinch zoom";

int g_calibrateVerticalCommandId = 0;
gaccel_register_t g_calibrateVerticalGaccel{};
std::string g_calibrateVerticalDesc = "Trackpad Engine: Calibrate vertical scroll";

int g_calibrateHorizontalCommandId = 0;
gaccel_register_t g_calibrateHorizontalGaccel{};
std::string g_calibrateHorizontalDesc = "Trackpad Engine: Calibrate mixer horizontal scroll";

bool g_hookCommandRegistered = false;
bool g_toggleActionRegistered = false;

void ConsoleLog(const char *msg) {
    if (ShowConsoleMsg) ShowConsoleMsg(msg);
}

bool ReadPersistentBool(const char *key, bool fallback) {
    if (!GetExtState) return fallback;
    const char *value = GetExtState(kSettingsSection, key);
    if (!value || !value[0]) return fallback;
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) return true;
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0) return false;
    return fallback;
}

void SaveGestureSettings() {
    if (!SetExtState) return;
    SetExtState(kSettingsSection, kHorizontalSetting,
                ReaperNavigation::IsHorizontalScrollEnabled() ? "1" : "0", true);
    SetExtState(kSettingsSection, kVerticalSetting,
                ReaperNavigation::IsVerticalScrollEnabled() ? "1" : "0", true);
    SetExtState(kSettingsSection, kZoomSetting,
                ReaperNavigation::IsZoomEnabled() ? "1" : "0", true);
}

void LoadGestureSettings() {
    ReaperNavigation::SetHorizontalScrollEnabled(
        ReadPersistentBool(kHorizontalSetting, false));
    ReaperNavigation::SetVerticalScrollEnabled(
        ReadPersistentBool(kVerticalSetting, false));
    ReaperNavigation::SetZoomEnabled(
        ReadPersistentBool(kZoomSetting, false));
}

bool AreAllGesturesEnabled() {
    return ReaperNavigation::IsHorizontalScrollEnabled() &&
           ReaperNavigation::IsVerticalScrollEnabled() &&
           ReaperNavigation::IsZoomEnabled();
}

void SetAllGesturesEnabled(bool enabled) {
    ReaperNavigation::SetHorizontalScrollEnabled(enabled);
    ReaperNavigation::SetVerticalScrollEnabled(enabled);
    ReaperNavigation::SetZoomEnabled(enabled);
    SaveGestureSettings();
}

bool RunCommand(int command, int /*flag*/) {
    if (command == g_toggleMasterCommandId) {
        bool enabled = !AreAllGesturesEnabled();
        SetAllGesturesEnabled(enabled);
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "[TrackpadEngine] complete gesture engine %s (state saved)\n",
                 enabled ? "ENABLED" : "DISABLED");
        ConsoleLog(buf);
        return true;
    }
    if (command == g_toggleLogCommandId) {
        Diagnostics::SetEnabled(!Diagnostics::IsEnabled());
        char buf[160];
        snprintf(buf, sizeof(buf), "[TrackpadEngine] diagnostic logging %s\n",
                 Diagnostics::IsEnabled() ? "ENABLED" : "DISABLED");
        ConsoleLog(buf);
        return true;
    }
    if (command == g_toggleScrollCommandId) {
        ReaperNavigation::SetHorizontalScrollEnabled(!ReaperNavigation::IsHorizontalScrollEnabled());
        SaveGestureSettings();
        char buf[160];
        snprintf(buf, sizeof(buf), "[TrackpadEngine] custom horizontal scroll %s\n",
                 ReaperNavigation::IsHorizontalScrollEnabled() ? "ENABLED" : "DISABLED");
        ConsoleLog(buf);
        return true;
    }
    if (command == g_toggleVerticalCommandId) {
        ReaperNavigation::SetVerticalScrollEnabled(!ReaperNavigation::IsVerticalScrollEnabled());
        SaveGestureSettings();
        char buf[160];
        snprintf(buf, sizeof(buf), "[TrackpadEngine] custom vertical scroll %s\n",
                 ReaperNavigation::IsVerticalScrollEnabled() ? "ENABLED" : "DISABLED");
        ConsoleLog(buf);
        return true;
    }
    if (command == g_toggleZoomCommandId) {
        ReaperNavigation::SetZoomEnabled(!ReaperNavigation::IsZoomEnabled());
        SaveGestureSettings();
        char buf[160];
        snprintf(buf, sizeof(buf), "[TrackpadEngine] custom pinch zoom %s\n",
                 ReaperNavigation::IsZoomEnabled() ? "ENABLED" : "DISABLED");
        ConsoleLog(buf);
        return true;
    }
    if (command == g_calibrateVerticalCommandId) {
        bool started = ReaperNavigation::BeginVerticalScrollCalibration();
        ConsoleLog(started
            ? "[TrackpadEngine] vertical calibration started (+1, then restore -1)\n"
            : "[TrackpadEngine] vertical calibration already running\n");
        return true;
    }
    if (command == g_calibrateHorizontalCommandId) {
        bool started = ReaperNavigation::BeginHorizontalScrollCalibration();
        ConsoleLog(started
            ? "[TrackpadEngine] mixer horizontal calibration started (+1, then restore -1)\n"
            : "[TrackpadEngine] mixer horizontal calibration already running\n");
        return true;
    }
    return false;
}

int ToggleActionState(int command) {
    if (command == g_toggleMasterCommandId) return AreAllGesturesEnabled() ? 1 : 0;
    if (command == g_toggleLogCommandId) return Diagnostics::IsEnabled() ? 1 : 0;
    if (command == g_toggleScrollCommandId) return ReaperNavigation::IsHorizontalScrollEnabled() ? 1 : 0;
    if (command == g_toggleVerticalCommandId) return ReaperNavigation::IsVerticalScrollEnabled() ? 1 : 0;
    if (command == g_toggleZoomCommandId) return ReaperNavigation::IsZoomEnabled() ? 1 : 0;
    return -1;
}

void TimerTick() {
    Diagnostics::Flush();

    // Advances any active synthesized pinch-zoom momentum (see
    // PinchZoomProcessor.h). No-op (returns a zeroed command, changes
    // nothing) when no momentum is active -- the common case. Reuses
    // ReaperNavigation::Apply exactly as a real event would: outContext is
    // the cached anchor/view info from the gesture that started the
    // momentum, filled in by MotionEngine::Tick. Return value ignored here
    // (unlike ViewSwizzleInterceptor's use of it) -- there's no original
    // Cocoa implementation to conditionally suppress on a timer tick.
    TrackpadEvent momentumContext;
    NavigationCommand momentumCommand = MotionEngine::Tick(momentumContext);
    if (momentumCommand.zoomAmount != 0.0) {
        ReaperNavigation::Apply(momentumContext, momentumCommand);
    }

    ReaperNavigation::VerticalScrollCalibrationResult calibration;
    if (ReaperNavigation::TickVerticalScrollCalibration(calibration)) {
        Diagnostics::DiagnosticEvent de;
        de.type = Diagnostics::DiagnosticEventType::VerticalCalibration;
        de.timestampSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        de.calibrationValid = calibration.valid;
        de.calibrationBeforeY = calibration.beforeY;
        de.calibrationAfterPositiveY = calibration.afterPositiveY;
        de.calibrationAfterRestoreY = calibration.afterRestoreY;
        Diagnostics::Capture(de);

        char buf[320];
        snprintf(buf, sizeof(buf),
                 "[TrackpadEngine] vertical calibration %s: before=%.3f after+1=%.3f "
                 "delta=%.3f restored=%.3f error=%.3f\n",
                 calibration.valid ? "complete" : "FAILED (no track)",
                 calibration.beforeY, calibration.afterPositiveY,
                 calibration.afterPositiveY - calibration.beforeY,
                 calibration.afterRestoreY,
                 calibration.afterRestoreY - calibration.beforeY);
        ConsoleLog(buf);
    }

    ReaperNavigation::HorizontalScrollCalibrationResult horizontalCalibration;
    if (ReaperNavigation::TickHorizontalScrollCalibration(horizontalCalibration)) {
        Diagnostics::DiagnosticEvent de;
        de.type = Diagnostics::DiagnosticEventType::MixerCalibration;
        de.timestampSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        de.mixerCalibrationBeforeX = horizontalCalibration.beforeMcpX;
        de.mixerCalibrationAfterPositiveX = horizontalCalibration.afterPositiveMcpX;
        de.mixerCalibrationAfterRestoreX = horizontalCalibration.afterRestoreMcpX;
        de.mixerCalibrationBeforeArrange = horizontalCalibration.beforeArrangeStart;
        de.mixerCalibrationAfterPositiveArrange =
            horizontalCalibration.afterPositiveArrangeStart;
        de.mixerCalibrationAfterRestoreArrange =
            horizontalCalibration.afterRestoreArrangeStart;
        Diagnostics::Capture(de);

        char buf[512];
        snprintf(buf, sizeof(buf),
                 "[TrackpadEngine] mixer horizontal calibration %s: "
                 "MCP before=%.3f after+1=%.3f delta=%.3f restored=%.3f error=%.3f; "
                 "Arrange before=%.9f after+1=%.9f delta=%.9f restored=%.9f error=%.9f\n",
                 horizontalCalibration.valid ? "complete" : "FAILED (no visible mixer track)",
                 horizontalCalibration.beforeMcpX,
                 horizontalCalibration.afterPositiveMcpX,
                 horizontalCalibration.afterPositiveMcpX - horizontalCalibration.beforeMcpX,
                 horizontalCalibration.afterRestoreMcpX,
                 horizontalCalibration.afterRestoreMcpX - horizontalCalibration.beforeMcpX,
                 horizontalCalibration.beforeArrangeStart,
                 horizontalCalibration.afterPositiveArrangeStart,
                 horizontalCalibration.afterPositiveArrangeStart - horizontalCalibration.beforeArrangeStart,
                 horizontalCalibration.afterRestoreArrangeStart,
                 horizontalCalibration.afterRestoreArrangeStart - horizontalCalibration.beforeArrangeStart);
        ConsoleLog(buf);
    }
}

void RegisterAction(int &commandId, gaccel_register_t &gaccel, const std::string &name,
                     const char *commandIdString) {
    commandId = plugin_register("command_id", (void *)commandIdString);
    if (commandId) {
        gaccel.accel.fVirt = 0;
        gaccel.accel.key = 0;
        gaccel.accel.cmd = (unsigned short)commandId;
        gaccel.desc = name.c_str();
        plugin_register("gaccel", &gaccel);
    }
}

void UnregisterAll() {
    if (g_hookCommandRegistered) {
        plugin_register("-hookcommand", (void *)RunCommand);
        g_hookCommandRegistered = false;
    }
    if (g_toggleActionRegistered) {
        plugin_register("-toggleaction", (void *)ToggleActionState);
        g_toggleActionRegistered = false;
    }
    plugin_register("-timer", (void *)TimerTick);
    if (g_toggleMasterCommandId) plugin_register("-gaccel", &g_toggleMasterGaccel);
    if (g_toggleLogCommandId) plugin_register("-gaccel", &g_toggleLogGaccel);
    if (g_toggleScrollCommandId) plugin_register("-gaccel", &g_toggleScrollGaccel);
    if (g_toggleVerticalCommandId) plugin_register("-gaccel", &g_toggleVerticalGaccel);
    if (g_toggleZoomCommandId) plugin_register("-gaccel", &g_toggleZoomGaccel);
    if (g_calibrateVerticalCommandId) plugin_register("-gaccel", &g_calibrateVerticalGaccel);
    if (g_calibrateHorizontalCommandId) plugin_register("-gaccel", &g_calibrateHorizontalGaccel);
}

} // namespace

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE /*hInstance*/,
                                                                   reaper_plugin_info_t *rec) {
    if (!rec) {
        // REAPER is unloading us: tear down in the reverse order of setup.
        // Removing the Cocoa hooks first is essential -- once this dylib's
        // code is unmapped, a leftover monitor block or swizzled IMP would
        // crash on the next scroll/pinch event (or the next event of
        // whatever selector was swizzled).
        ViewSwizzleInterceptor::Remove();
        TrackpadInterceptor::Remove();
        UnregisterAll();
        Diagnostics::Shutdown();
        return 0;
    }

    if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc) {
        return 0;
    }
    if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) {
        // One of the functions listed via REAPERAPI_WANT_* above failed to
        // resolve -- refuse to load rather than run with null function
        // pointers.
        return 0;
    }

    std::string logBaseDir = std::string(GetResourcePath()) + "/TrackpadEngine";
    bool logOk = Diagnostics::Init(logBaseDir.c_str());
    LoadGestureSettings();

    RegisterAction(g_toggleMasterCommandId, g_toggleMasterGaccel, g_toggleMasterDesc,
                   "TRACKPADENGINE_TOGGLE_MASTER");
    RegisterAction(g_toggleLogCommandId, g_toggleLogGaccel, g_toggleLogDesc,
                   "TRACKPADENGINE_TOGGLE_LOG");
    RegisterAction(g_toggleScrollCommandId, g_toggleScrollGaccel, g_toggleScrollDesc,
                   "TRACKPADENGINE_TOGGLE_HSCROLL");
    RegisterAction(g_toggleVerticalCommandId, g_toggleVerticalGaccel, g_toggleVerticalDesc,
                   "TRACKPADENGINE_TOGGLE_VSCROLL");
    RegisterAction(g_toggleZoomCommandId, g_toggleZoomGaccel, g_toggleZoomDesc,
                   "TRACKPADENGINE_TOGGLE_ZOOM");
    RegisterAction(g_calibrateVerticalCommandId, g_calibrateVerticalGaccel,
                   g_calibrateVerticalDesc, "TRACKPADENGINE_CALIBRATE_VSCROLL");
    RegisterAction(g_calibrateHorizontalCommandId, g_calibrateHorizontalGaccel,
                   g_calibrateHorizontalDesc, "TRACKPADENGINE_CALIBRATE_MIXER_HSCROLL");
    g_hookCommandRegistered = plugin_register("hookcommand", (void *)RunCommand) != 0;
    g_toggleActionRegistered = plugin_register("toggleaction", (void *)ToggleActionState) != 0;
    plugin_register("timer", (void *)TimerTick);

    bool interceptorOk = TrackpadInterceptor::Install();
    bool viewSwizzleOk = ViewSwizzleInterceptor::Install((void *)rec->hwnd_main);

    char buf[1280];
    snprintf(buf, sizeof(buf),
             "[TrackpadEngine] loaded (REAPER %s). log file=%s\n"
             "[TrackpadEngine] app monitor=%s. view swizzle=%s (scrollWheel: hooked on %d class(es), "
             "magnifyWithEvent: hooked on %d class(es))\n"
             "[TrackpadEngine] complete gesture engine: %s (persistent).\n"
             "[TrackpadEngine] horizontal/vertical/pinch: %s / %s / %s.\n"
             "[TrackpadEngine] main action: \"%s\". Advanced actions remain available.\n",
             GetAppVersion(), logOk ? "ok" : "FAILED",
             interceptorOk ? "installed" : "FAILED",
             viewSwizzleOk ? "installed" : "FAILED",
             ViewSwizzleInterceptor::ScrollHookCount(), ViewSwizzleInterceptor::MagnifyHookCount(),
             AreAllGesturesEnabled() ? "ENABLED" : "DISABLED/PARTIAL",
             ReaperNavigation::IsHorizontalScrollEnabled() ? "ON" : "OFF",
             ReaperNavigation::IsVerticalScrollEnabled() ? "ON" : "OFF",
             ReaperNavigation::IsZoomEnabled() ? "ON" : "OFF",
             g_toggleMasterDesc.c_str());
    ConsoleLog(buf);

    return 1;
}
