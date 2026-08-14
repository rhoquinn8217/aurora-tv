# ⭐ OUR COPY, AND IT DELIBERATELY SHADOWS THE ONE IN third_party/commons.
#
# `CMAKE_MODULE_PATH` puts this project's own cmake/ directory FIRST, ahead of
# the submodule's, so `include(ExternalSDL2BackportForWebOS)` finds this file
# instead. Nothing in the submodule is edited -- which keeps it a submodule.
#
# ⛔ WHY WE NEED OUR OWN SDL AT ALL.
#
# SDL's DualSense driver misreads microphone audio as controller input.
#
# A DualSense over Bluetooth sends its pad state in report 0x31 at 78 bytes.
# When its microphone is streaming it ALSO sends audio in report 0x31 at 78
# bytes -- same id, same length -- with one bit in the tag byte to tell them
# apart. Bit 0 means "controller state is present"; audio-only reports leave
# it clear.
#
# SDL never checks that bit. It validates the CRC, which passes, and parses
# the audio as sticks, buttons and touchpad -- several hundred phantom inputs
# a second. Measured on hardware 2026-08-13: a mouse crossing a desktop on its
# own, and menus activating themselves until the controller was powered off.
#
# ⚠️ The Linux kernel's hid-playstation driver has the same omission, so
# switching SDL to the kernel path is not an escape. And as far as a search of
# SDL's issues, the kernel lists and the forums can establish, NOBODY HAS
# REPORTED THIS. It is a real bug and the fix is one line.
#
# ➡️ See bt-microphone-findings.md, and the patch itself in the fork.
#
# ⭐ THE UPSTREAM MODULE ALREADY SUPPORTS BUILDING FROM SOURCE -- it takes
# either SDL2_BACKPORT_RELEASE (download a prebuilt tarball) or
# SDL2_BACKPORT_REVISION (clone and build). All this file changes is WHICH
# repository the source comes from, plus a local-directory option so a normal
# build does not pay for compiling SDL every time.

if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif ()
include(ExternalProject)

set(EXT_SDL2_BACKPORT_TOOLCHAIN_ARGS)
if (CMAKE_TOOLCHAIN_FILE)
    list(APPEND EXT_SDL2_BACKPORT_TOOLCHAIN_ARGS "-DCMAKE_TOOLCHAIN_FILE:string=${CMAKE_TOOLCHAIN_FILE}")
endif ()
if (CMAKE_TOOLCHAIN_ARGS)
    list(APPEND EXT_SDL2_BACKPORT_TOOLCHAIN_ARGS "-DCMAKE_TOOLCHAIN_ARGS:string=${CMAKE_TOOLCHAIN_ARGS}")
endif ()

set(LIB_FILENAME "libSDL2-2.0.so.0")

# ⭐ THE PATH A NORMAL BUILD TAKES: use an SDL we built EARLIER and kept.
#
# ⛔ The build directory is wiped on every run -- `rm -rf ${CMAKE_BINARY_DIR}`
# in docker_build_inner.sh, which exists because cmake's try_compile breaks on
# Windows filesystems. So anything ExternalProject clones or compiles is
# thrown away and done again next time, and SDL is not small.
#
# Building it once and pointing at the result keeps ordinary builds exactly as
# fast as they were with the prebuilt tarball. SDL is rebuilt only when the
# patch changes, by running the helper script rather than as a side effect of
# every build.
if (DEFINED SDL2_BACKPORT_PREBUILT_DIR)
    if (NOT EXISTS "${SDL2_BACKPORT_PREBUILT_DIR}/lib/${LIB_FILENAME}")
        message(FATAL_ERROR
                "SDL2_BACKPORT_PREBUILT_DIR is set to '${SDL2_BACKPORT_PREBUILT_DIR}' "
                "but ${LIB_FILENAME} is not there. Build it first with "
                "scripts/build-sdl-fork.sh, or unset the variable to build from source.")
    endif ()
    add_custom_target(ext_sdl2_backport)     # nothing to do; it is already built
    set(INSTALL_DIR "${SDL2_BACKPORT_PREBUILT_DIR}")
    message(STATUS "SDL2: using our patched build at ${INSTALL_DIR}")

elseif (DEFINED SDL2_BACKPORT_REVISION)
    # ⚠️ Builds SDL from OUR fork. Correct, and slow on every clean build --
    # which is every build. Use it to produce the prebuilt directory above,
    # not as the everyday path.
    if (NOT DEFINED SDL2_BACKPORT_REPOSITORY)
        message(FATAL_ERROR "SDL2_BACKPORT_REPOSITORY is not defined")
    endif ()
    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        set(EXT_SDL2_BACKPORT_BUILD_TYPE "Release")
    else ()
        set(EXT_SDL2_BACKPORT_BUILD_TYPE "RelWithDebInfo")
    endif ()
    ExternalProject_Add(ext_sdl2_backport
            GIT_REPOSITORY "${SDL2_BACKPORT_REPOSITORY}"
            GIT_TAG "${SDL2_BACKPORT_REVISION}"
            CMAKE_ARGS ${EXT_SDL2_BACKPORT_TOOLCHAIN_ARGS}
            -DCMAKE_BUILD_TYPE:string=${EXT_SDL2_BACKPORT_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
            -DWEBOS=ON -DSDL_OFFSCREEN=OFF -DSDL_DISKAUDIO=OFF
            -DSDL_DUMMYAUDIO=OFF -DSDL_DUMMYVIDEO=OFF -DSDL_KMSDRM=OFF
            -DSDL_VENDOR_INFO=webOS\ Backport
            BUILD_BYPRODUCTS <INSTALL_DIR>/lib/${LIB_FILENAME}
    )
    ExternalProject_Get_Property(ext_sdl2_backport INSTALL_DIR)
    message(STATUS "SDL2: building from ${SDL2_BACKPORT_REPOSITORY} @ ${SDL2_BACKPORT_REVISION}")

else ()
    message(FATAL_ERROR
            "Set SDL2_BACKPORT_PREBUILT_DIR (normal) or SDL2_BACKPORT_REVISION "
            "with SDL2_BACKPORT_REPOSITORY (to build the fork).")
endif ()

# ⓘ From here down this matches the upstream module exactly, so a reader can
# diff the two and see that only the source selection differs.
add_library(ext_sdl2_backport_target SHARED IMPORTED)
set_target_properties(ext_sdl2_backport_target PROPERTIES IMPORTED_LOCATION ${INSTALL_DIR}/lib/${LIB_FILENAME})
target_compile_definitions(ext_sdl2_backport_target INTERFACE __WEBOS__)
add_dependencies(ext_sdl2_backport_target ext_sdl2_backport)
set(SDL2_INCLUDE_DIRS ${INSTALL_DIR}/include/SDL2)
set(SDL2_LIBRARIES ext_sdl2_backport_target)
set(SDL2_FOUND TRUE)
if (NOT DEFINED CMAKE_INSTALL_LIBDIR)
    set(CMAKE_INSTALL_LIBDIR lib)
endif ()
install(DIRECTORY ${INSTALL_DIR}/lib/ DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PATTERN "include" EXCLUDE PATTERN "bin" EXCLUDE PATTERN "share" EXCLUDE
        PATTERN "pkgconfig" EXCLUDE PATTERN "cmake" EXCLUDE PATTERN "*.a" EXCLUDE)
