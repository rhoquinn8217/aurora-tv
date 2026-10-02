#!/usr/bin/env bash
# Build the webOS package.
#
# ═══════════════════════════════════════════════════════════════════════════
# WHY THIS EXISTS
# ═══════════════════════════════════════════════════════════════════════════
#
# It checks that the bridge core is in the checkout, tells the build which
# commit of the core it is compiling, and keeps the toolchain between builds.
#
# ⭐ THE BRIDGE CORE IS A SUBMODULE: third_party/ctm-bridge-webos. Each commit
# of this repository records the exact commit of the core it is built with,
# and this app compiles it straight from that folder. A clone made with
# --recursive has it; in any other checkout
#
#     git submodule update --init --recursive
#
# fetches it. Nothing has to sit next to this checkout.
#
# ⓘ THE APP REPORTS THE CORE IT WAS BUILT FROM: `core=` in the reply to
# `status` on its command port (src/app/control_server.h). The folder is
# compiled as it stands, so a folder that is not at the commit this checkout
# records, or that holds changes in no commit, is named as such here and in
# what the app reports.
#
# ═══════════════════════════════════════════════════════════════════════════
# MOUNTS
# ═══════════════════════════════════════════════════════════════════════════
#
# ../.build-cache/tmp   -> /tmp                 created if missing
#
#   The webOS toolchain is downloaded and unpacked into /tmp inside the
#   container, which does not survive it. Without this mount every build
#   re-downloads the whole SDK.
#
# ═══════════════════════════════════════════════════════════════════════════
#
#   ./scripts/build-ipk.sh
#
# Requires Docker. Everything else is fetched inside the container.

set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

# ── The bridge core is required ─────────────────────────────────────────────
CORE_PATH="third_party/ctm-bridge-webos"
CORE="$ROOT/$CORE_PATH"
if [ ! -f "$CORE/src/app/ctm_state.c" ]; then
    echo "ERROR: the bridge core is not in this checkout."
    echo
    echo "  expected: $CORE"
    echo
    echo "  It is a submodule of this repository. Fetch it with:"
    echo
    echo "      git submodule update --init --recursive"
    exit 1
fi

# ── Which commit of the core this build compiles ────────────────────────────
# Read off the folder itself, since the folder is what gets compiled. Without
# a .git of its own (an unpacked archive, say) git would answer for the
# repository around it, so nothing is claimed.
CORE_COMMIT="unknown"
if [ -e "$CORE/.git" ]; then
    CORE_COMMIT="$(git -C "$CORE" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
    if [ -n "$(git -C "$CORE" status --porcelain 2>/dev/null)" ]; then
        CORE_COMMIT="$CORE_COMMIT+uncommitted"
    fi
fi
RECORDED="$(git -C "$ROOT" rev-parse -q --verify "HEAD:$CORE_PATH" 2>/dev/null | cut -c1-12)"

# ── The toolchain cache ─────────────────────────────────────────────────────
mkdir -p "$ROOT/../.build-cache/tmp"

echo "building the webOS package"
echo "  bridge core:  $CORE_COMMIT"
if [ -n "$RECORDED" ] && [ "$CORE_COMMIT" != "$RECORDED" ]; then
    echo "  WARNING: this checkout records $RECORDED. The folder is built as it"
    echo "           stands; \`git submodule update\` puts it back on that commit."
fi
echo

docker run --rm --platform linux/amd64 \
  --dns 8.8.8.8 --dns 1.1.1.1 \
  -v "$ROOT:/build" \
  -v "$ROOT/scripts/webos/docker_build_inner.sh:/docker_build.sh" \
  -v "$ROOT/../.build-cache/tmp:/tmp" \
  -w /build -e CI=1 -e DOCKER_SKIP_SUBMODULES=1 \
  -e BRIDGE_CORE_COMMIT="$CORE_COMMIT" \
  ubuntu:22.04 \
  bash -c "sed 's/\r\$//' /docker_build.sh | bash"

echo
echo "Package:"
ls -la "$ROOT"/dist/*_arm.ipk
