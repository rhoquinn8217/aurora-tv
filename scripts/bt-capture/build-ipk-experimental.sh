#!/usr/bin/env bash
# Build the webOS package ON THE mic-capture-experimental BRANCH.
#
# ⛔⛔ EXPERIMENTAL BRANCH ONLY. This file does not exist on the stable branch,
# on purpose -- so it can never conflict when stable is merged forward, and so
# stable's tree carries nothing about capture.
#
# ⭐ USE THIS ONE HERE, NOT scripts/build-ipk.sh. That script is stable's and
# gets inherited by every merge forward. It does NOT mount the patched SDL, so
# building with it on this branch produces a package with no guard in it.
#
# ═══════════════════════════════════════════════════════════════════════════
# WHAT THIS ADDS OVER THE STABLE SCRIPT
# ═══════════════════════════════════════════════════════════════════════════
#
# One mount: ../sdl-webos-patched -> /sdl-patched
#
# This branch builds against a patched SDL whose PlayStation driver checks the
# flag saying whether a report carries controller state or microphone audio.
# ⛔ Without it, an armed controller's audio is parsed as sticks and buttons --
# several hundred phantom inputs a second.
#
# ⚠️ CMakeLists.txt on this branch sets SDL2_BACKPORT_PREBUILT_DIR to
# /sdl-patched unconditionally and the cmake module errors when the library is
# absent. That is deliberate: an unpatched build of this branch is not safe to
# run, so it refuses to build rather than producing one quietly.
#
# ═══════════════════════════════════════════════════════════════════════════
# BUILD THE PATCHED SDL FIRST -- ONCE
# ═══════════════════════════════════════════════════════════════════════════
#
#   ./scripts/build-sdl-fork.sh
#
# It builds rhoquinn8217/SDL-webOS at tag release-2.30.12-webos.5-micflag into
# ../sdl-webos-patched, beside this checkout. ⭐ Once only -- rebuild it only
# when the patch changes. Building it as part of every app build costs ~500
# seconds each time, because the inner build script wipes its build directory
# every run.
#
# ═══════════════════════════════════════════════════════════════════════════
#
#   ./scripts/bt-capture/build-ipk-experimental.sh
#
# Requires Docker. Everything else is fetched inside the container.

set -e
cd "$(dirname "$0")/../.."
ROOT="$(pwd)"

# ── The bridge core is required ─────────────────────────────────────────────
CORE="$ROOT/../ctm-bridge-webos"
if [ ! -f "$CORE/src/app/ctm_state.c" ]; then
    echo "ERROR: the bridge core was not found next to this checkout."
    echo
    echo "  expected: $CORE"
    echo
    echo "  git clone https://github.com/rhoquinn8217/ctm-bridge-webos.git \\"
    echo "      \"$ROOT/../ctm-bridge-webos\""
    echo
    echo "  ⚠️ Check out mic-capture-experimental there too -- the arming code"
    echo "     lives in the core, not in this repository."
    exit 1
fi

# ── The patched SDL is required on this branch ──────────────────────────────
SDL="$ROOT/../sdl-webos-patched"
if [ ! -d "$SDL" ]; then
    echo "ERROR: the patched SDL has not been built."
    echo
    echo "  expected: $SDL"
    echo
    echo "  ./scripts/build-sdl-fork.sh"
    echo
    echo "  ⛔ This branch will not build without it, and that is deliberate:"
    echo "     an unpatched build of this branch is not safe to run."
    exit 1
fi

mkdir -p "$ROOT/../.build-cache/tmp"

echo "building the webOS package (experimental)"
echo "  bridge core:  $CORE"
echo "  patched SDL:  $SDL"
echo

docker run --rm --platform linux/amd64 \
  --dns 8.8.8.8 --dns 1.1.1.1 \
  -v "$ROOT:/build" \
  -v "$CORE:/ctm-bridge-webos" \
  -v "$ROOT/scripts/webos/docker_build_inner.sh:/docker_build.sh" \
  -v "$ROOT/../.build-cache/tmp:/tmp" \
  -v "$SDL:/sdl-patched" \
  -w /build -e CI=1 -e DOCKER_SKIP_SUBMODULES=1 \
  ubuntu:22.04 \
  bash -c "sed 's/\r\$//' /docker_build.sh | bash"

echo
echo "Package:"
ls -la "$ROOT"/dist/*_arm.ipk

# ⭐ THE PACKAGE SIZE TELLS YOU WHICH SDL WENT IN, with nothing to read:
#   ~4.05 MB  the patched SDL is linked in     -- correct for this branch
#   ~2.18 MB  the stock backport is linked in  -- ⛔ THE GUARD IS NOT IN IT
#
# ⛔ If you get the smaller size on this branch, something built with stable's
# configuration. Check that CMakeLists.txt still contains sdl-patched -- a
# merge from stable can take its side of that block.
