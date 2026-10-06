# CTM bridge core, embedded into moonlight-tv as a headless static lib.
#
# Compiles the bridge's headless core from the ctm-bridge-webos repo (this
# fork's own, forked from github.com/CTM-Bridge/ctm-bridge-webos; the
# stand-alone TV app and this embeddable core are the same sources), MINUS
# enet_transport.c: moonlight has its own bundled ENet, so we exclude the
# bridge's ENet and satisfy the enet_client_* references with enet_stub.c
# (TCP-only at runtime). No LVGL/SDL.
#
# The core is a SUBMODULE of this repo, at third_party/ctm-bridge-webos, as the
# app's other dependencies are. It used to be read from a folder beside this
# checkout, which a clone of this repo does not have, and whatever that folder
# held was built, committed or not. As a submodule, every commit here records
# the exact commit of the core it is built with, and nothing else is used.
# The location can still be overridden with -DCTM_BRIDGE_DIR=<path>.
if(NOT DEFINED CTM_BRIDGE_DIR)
    set(CTM_BRIDGE_DIR "${CMAKE_SOURCE_DIR}/third_party/ctm-bridge-webos")
endif()

if(NOT EXISTS "${CTM_BRIDGE_DIR}/src/app/ctm_state.c")
    message(FATAL_ERROR "CTM bridge core not found at ${CTM_BRIDGE_DIR}. "
                        "It is a submodule: run `git submodule update --init --recursive`, "
                        "or set -DCTM_BRIDGE_DIR=<path> to build against another copy.")
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

# The commit of the core this build is made from. The app reports it (the
# control port's `status`, as core=), and a new value changes this target's
# flags, so every file of the core is compiled again when the recorded commit
# changes, whatever the files' times say. build-aurora-docker.ps1 passes it in,
# since inside its container git cannot read this repo; elsewhere it is read
# from git here, and it is "unknown" where neither works.
if(DEFINED ENV{BRIDGE_CORE_COMMIT} AND NOT "$ENV{BRIDGE_CORE_COMMIT}" STREQUAL "")
    set(BRIDGE_CORE_COMMIT "$ENV{BRIDGE_CORE_COMMIT}")
else()
    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse HEAD:third_party/ctm-bridge-webos
                    OUTPUT_VARIABLE BRIDGE_CORE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE BRIDGE_CORE_COMMIT_RC)
    if(BRIDGE_CORE_COMMIT_RC EQUAL 0 AND BRIDGE_CORE_COMMIT MATCHES "^[0-9a-f]+$")
        string(SUBSTRING "${BRIDGE_CORE_COMMIT}" 0 12 BRIDGE_CORE_COMMIT)
    else()
        set(BRIDGE_CORE_COMMIT "unknown")
    endif()
endif()
target_compile_definitions(ctmbridge PRIVATE BRIDGE_CORE_COMMIT="${BRIDGE_CORE_COMMIT}")
message(STATUS "Bridge core: ${CTM_BRIDGE_DIR}, commit ${BRIDGE_CORE_COMMIT}")

# (the core .c files that need it self-#define _GNU_SOURCE; no global define here)
target_link_libraries(ctmbridge PUBLIC pthread m)

target_link_libraries(moonlight-lib PUBLIC ctmbridge)

# Everything else this fork adds to the build is in two files beside this one,
# so that upstream's own build files stay exactly as upstream wrote them.
include(${CMAKE_CURRENT_LIST_DIR}/app_sources.cmake)
# The fork's build number is apart from the bridge itself, and a tree without
# that file builds with upstream's version and nothing else changed.
include(${CMAKE_CURRENT_LIST_DIR}/build_number.cmake OPTIONAL)
