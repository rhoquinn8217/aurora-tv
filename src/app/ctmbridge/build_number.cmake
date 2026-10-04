# This fork's build number, and how it reaches the version the app shows.
#
# The version is upstream's: MOONLIGHT_VERSION in the root CMakeLists.txt, which
# follows GuiDev1994's releases as they are merged forward. The build number is
# this fork's alone, and it keeps counting across a merge. Resetting it would
# make old builds and new ones share numbers, and every log and test note that
# names one would become ambiguous. The app shows the two together, as
# "1.3.0 (N)". appinfo.json and the package stay at the numeric version.
#
# It lives here, and not beside MOONLIGHT_VERSION where it used to, so that a
# build changes no file of upstream's. On the line under the version it
# conflicted with every upstream release that was merged.
#
# !! build-aurora-docker.ps1 REWRITES THE NEXT LINE AND COMMITS IT AS "Build N".
# !! It finds the line by scanning this file for `set(CTM_BUILD_NUMBER <digits>)`,
# !! once to pick the next number and once to name the package. Keep it on one
# !! line and digits only: a letter inside the value would break both scans, and
# !! the next build would silently reuse a number.
set(CTM_BUILD_NUMBER 480)

# A tag after the build number, so a build you can SEE says what it is: EXP on
# an experimental branch, such as the DualSense Bluetooth microphone capture.
# A separate variable for the reason above. A branch sets it here, and
# build-aurora-docker.ps1 -Suffix names the package to match.
set(CTM_BUILD_SUFFIX "")

# The version with the build number, as a generated header. See the template
# for why it is a header and not a compile flag.
configure_file(${CMAKE_SOURCE_DIR}/src/app/app_version.h.in ${CMAKE_BINARY_DIR}/app_version.h @ONLY)

# Every file that shows a version gets that header ahead of its own text, on
# its command line. Upstream's APP_VERSION flag and upstream's files are left
# exactly as they are: nothing in them includes the header or knows about it.
#
# !! A file that shows a version and is NOT listed here shows upstream's, with
# !! no build number. tests/merge-guard.sh compares this list with the tree.
set(VERSION_SOURCES
        ${CMAKE_SOURCE_DIR}/src/app/app.c
        ${CMAKE_SOURCE_DIR}/src/app/control_server.c
        ${CMAKE_SOURCE_DIR}/src/app/ui/help/help.dialog.c)
if (MSVC)
    set(VERSION_HEADER_OPTION "/FI${CMAKE_BINARY_DIR}/app_version.h")
else ()
    set(VERSION_HEADER_OPTION "-include;${CMAKE_BINARY_DIR}/app_version.h")
endif ()
set_source_files_properties(${VERSION_SOURCES} TARGET_DIRECTORY moonlight-lib
        PROPERTIES COMPILE_OPTIONS "${VERSION_HEADER_OPTION}")
