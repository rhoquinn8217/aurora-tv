# The app-side sources the bridge adds to moonlight-lib.
#
# They sit in upstream's folders, beside the code they work with, but they are
# listed HERE and not in each folder's own CMakeLists.txt. Upstream's lists
# stay exactly as upstream wrote them, so a release that reorders or rewrites
# one cannot drop a file of ours, and cannot conflict with one either.
#
# !! A new source file of the fork's goes in this list. tests/merge-guard.sh
# !! checks that every one named here still exists.
target_sources(moonlight-lib PRIVATE
        ${CMAKE_SOURCE_DIR}/src/app/bridge_app.c
        ${CMAKE_SOURCE_DIR}/src/app/control_server.c
        ${CMAKE_SOURCE_DIR}/src/app/input/auto_bridge.c
        ${CMAKE_SOURCE_DIR}/src/app/input/bridge_keyboard.c
        ${CMAKE_SOURCE_DIR}/src/app/input/bridge_override.c
        ${CMAKE_SOURCE_DIR}/src/app/input/bridge_request.c
        ${CMAKE_SOURCE_DIR}/src/app/input/ctm_bridge_gesture.c
        ${CMAKE_SOURCE_DIR}/src/app/input/device_groups.c
        ${CMAKE_SOURCE_DIR}/src/app/ui/settings/auto_bridge_window.c
        ${CMAKE_SOURCE_DIR}/src/app/ui/settings/panes/usbbridge.pane.c
        ${CMAKE_SOURCE_DIR}/src/app/ui/streaming/bridge_prompt.c
        ${CMAKE_SOURCE_DIR}/src/app/ui/streaming/ctm_panel.c)

# A command port on 127.0.0.1 that lets a terminal on the TV launch a stream
# and bridge devices without the remote. Loopback only; see
# src/app/control_server.h.
option(AURORA_TERMINAL_CONTROL "Listen on 127.0.0.1 for commands from a terminal on the TV" ON)
if (AURORA_TERMINAL_CONTROL)
    target_compile_definitions(moonlight-lib PUBLIC AURORA_TERMINAL_CONTROL=1)
endif ()
