cmake_minimum_required(VERSION 3.20)

if(NOT APPLE)
    message(FATAL_ERROR "ReaperTrackpadEngine currently targets macOS only (Phase 1 scope).")
endif()

project(ReaperTrackpadEngine LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Multiple extensions share REAPER's process; keep our symbol surface to the
# one exported entry point (reaper_plugin.h marks it
# __attribute__((visibility("default"))) explicitly) and hide everything else.
# Set via a raw compile option (not just CMAKE_CXX_VISIBILITY_PRESET) because
# CMAKE_CXX_VISIBILITY_PRESET only covers the CXX language, not OBJCXX --
# TrackpadInterceptor.mm needs -fvisibility=hidden applied explicitly too.
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)

set(REAPER_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reaper-sdk")

add_library(reaper_trackpadengine SHARED
    src/PluginEntry.cpp
    src/Diagnostics.cpp
    src/MotionEngine.cpp
    src/ScrollGestureProcessor.cpp
    src/HorizontalScrollProcessor.cpp
    src/VerticalScrollProcessor.cpp
    src/PinchZoomProcessor.cpp
    src/ReaperNavigation.cpp
    src/CocoaEventUtil.mm
    src/TrackpadInterceptor.mm
    src/ViewSwizzleInterceptor.mm
)

target_include_directories(reaper_trackpadengine PRIVATE
    ${REAPER_SDK_ROOT}/sdk
)

target_compile_options(reaper_trackpadengine PRIVATE -fvisibility=hidden)

# All Cocoa-facing files use ARC.
set_source_files_properties(
    src/CocoaEventUtil.mm
    src/TrackpadInterceptor.mm
    src/ViewSwizzleInterceptor.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc"
)

target_link_libraries(reaper_trackpadengine PRIVATE "-framework Cocoa")

# REAPER on macOS only auto-loads UserPlugins files named "reaper_*.dylib"
# (no "lib" prefix, no versioned suffix).
set_target_properties(reaper_trackpadengine PROPERTIES
    PREFIX ""
    OUTPUT_NAME "reaper_trackpadengine"
    SUFFIX ".dylib"
)
