#!/usr/bin/env bash
# Build the webOS package.
#
# ═══════════════════════════════════════════════════════════════════════════
# WHY THIS EXISTS
# ═══════════════════════════════════════════════════════════════════════════
#
# The stock webOS build in scripts/webos/ does not know about the CTM bridge.
#
# ⛔ THE BRIDGE CORE IS A SIBLING CHECKOUT, NOT A SUBMODULE. This app compiles
# it straight from ../ctm-bridge-webos, so that repository must be cloned NEXT
# TO this one and mounted into the build container. Nothing fetches it for you,
# and the stock scripts do not mount it -- a build without it stops in cmake
# with a message about CTM_BRIDGE_DIR.
#
# ⚠️ A CONSEQUENCE WORTH KNOWING: because the core is found by path rather than
# by a pinned commit, a build of this app records NOTHING about which version
# of the core went into it. If you are testing, note both commits by hand.
#
# ═══════════════════════════════════════════════════════════════════════════
# MOUNTS
# ═══════════════════════════════════════════════════════════════════════════
#
# ../ctm-bridge-webos   -> /ctm-bridge-webos    required
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
CORE="$ROOT/../ctm-bridge-webos"
if [ ! -f "$CORE/src/app/ctm_state.c" ]; then
    echo "ERROR: the bridge core was not found next to this checkout."
    echo
    echo "  expected: $CORE"
    echo
    echo "  git clone https://github.com/rhoquinn8217/ctm-bridge-webos.git \\"
    echo "      \"$ROOT/../ctm-bridge-webos\""
    echo
    echo "  Check out the branch that matches this one."
    exit 1
fi

# ── The toolchain cache ─────────────────────────────────────────────────────
mkdir -p "$ROOT/../.build-cache/tmp"

echo "building the webOS package"
echo "  bridge core:  $CORE"
echo

docker run --rm --platform linux/amd64 \
  --dns 8.8.8.8 --dns 1.1.1.1 \
  -v "$ROOT:/build" \
  -v "$CORE:/ctm-bridge-webos" \
  -v "$ROOT/scripts/webos/docker_build_inner.sh:/docker_build.sh" \
  -v "$ROOT/../.build-cache/tmp:/tmp" \
  -w /build -e CI=1 -e DOCKER_SKIP_SUBMODULES=1 \
  ubuntu:22.04 \
  bash -c "sed 's/\r\$//' /docker_build.sh | bash"

echo
echo "Package:"
ls -la "$ROOT"/dist/*_arm.ipk
