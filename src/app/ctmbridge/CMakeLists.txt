# CTM bridge core, embedded into moonlight-tv as a headless static lib.
#
# Compiles the bridge's headless core straight from the sibling ctm-bridge-webos
# repo (github.com/CTM-Bridge/ctm-bridge-webos — the ONE bridge project: the
# standalone TV app and this embeddable core are the same sources), MINUS
# enet_transport.c: moonlight has its own bundled ENet, so we exclude the
# bridge's ENet and satisfy the enet_client_* references with enet_stub.c
# (TCP-only at runtime). No LVGL/SDL.
#
# The sibling repo location can be overridden with -DCTM_BRIDGE_DIR=...
if(NOT DEFINED CTM_BRIDGE_DIR)
    set(CTM_BRIDGE_DIR "${CMAKE_SOURCE_DIR}/../ctm-bridge-webos")
endif()

if(NOT EXISTS "${CTM_BRIDGE_DIR}/src/app/ctm_state.c")
    message(FATAL_ERROR "CTM bridge core not found at ${CTM_BRIDGE_DIR} "
                        "(expected the ctm-bridge-webos repo cloned next to this one). "
                        "Set -DCTM_BRIDGE_DIR=<path> to override.")
endif()

add_library(ctmbridge STATIC
    ${CTM_BRIDGE_DIR}/src/app/ctm_state.c
    ${CTM_BRIDGE_DIR}/src/app/ui_devices.c
    ${CTM_BRIDGE_DIR}/src/app/ui_bridge.c
    ${CTM_BRIDGE_DIR}/src/shared/ctm_hid.c
    ${CTM_BRIDGE_DIR}/src/shared/ctm_hostmouse.c
    ${CTM_BRIDGE_DIR}/src/shared/ctm_transport.c
    ${CTM_BRIDGE_DIR}/src/controllers/controller_common.c
    ${CTM_BRIDGE_DIR}/src/controllers/controller_ds5.c
    ${CTM_BRIDGE_DIR}/src/controllers/controller_ds4.c
    ${CTM_BRIDGE_DIR}/src/controllers/controller_xbox.c
    ${CTM_BRIDGE_DIR}/src/controllers/controller_xpad.c
    ${CTM_BRIDGE_DIR}/src/controllers/controller_steam_puck.c
    ${CTM_BRIDGE_DIR}/src/controllers/controller_generic.c
    ${CTM_BRIDGE_DIR}/src/monitor/ctm_monitor.c
    enet_stub.c
    ctm_bridge_glue.c
)

# ctm core headers are PRIVATE so they don't leak into moonlight-lib's compile;
# the glue header (added later) is exposed via this dir (PUBLIC).
target_include_directories(ctmbridge PRIVATE
    ${CTM_BRIDGE_DIR}/src
    ${CTM_BRIDGE_DIR}/src/app
    ${CTM_BRIDGE_DIR}/src/shared
    ${CTM_BRIDGE_DIR}/src/controllers
    ${CTM_BRIDGE_DIR}/src/monitor
)
target_include_directories(ctmbridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# (the core .c files that need it self-#define _GNU_SOURCE; no global define here)
target_link_libraries(ctmbridge PUBLIC pthread m)

target_link_libraries(moonlight-lib PUBLIC ctmbridge)
