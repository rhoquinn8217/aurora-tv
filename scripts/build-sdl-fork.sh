#!/usr/bin/env bash
# ⛔⛔⛔ EXPERIMENTAL BRANCH ONLY -- mic-capture-experimental.
#
# ⚠️ IF YOU ARE READING THIS AS A MERGE CONFLICT, THE ANSWER IS: KEEP THIS FILE.
#
# THE CONFLICT IS DELIBERATE, AND THIS COMMENT IS WHY IT EXISTS.
#
# The stable branch DELETED this file (2026-08-15, "Build against the SDL webOS
# ships, not our own"). Experimental keeps it, because the patched SDL is what
# stops an armed controller's microphone audio being parsed as sticks and
# buttons -- and arming only exists on this branch.
#
# ⛔ Without this comment the deletion would merge SILENTLY. Git only stops for
# a modify/delete conflict when BOTH sides touched the file. So this file is
# modified here on purpose, so that every merge from stable has to STOP and ask
# rather than quietly removing the thing this branch exists to hold.
#
# ➡️ ON A MERGE CONFLICT HERE: `git checkout --ours <this file>` and carry on.
#
# ⚠️ IF YOU EVER DELETE THIS COMMENT, the protection goes with it.
#
# Build our patched SDL once, and keep the result.
#
# ⛔ WHY THIS EXISTS RATHER THAN LETTING CMAKE DO IT.
#
# docker_build_inner.sh wipes the build directory on every run -- deliberately,
# because cmake's try_compile breaks on Windows filesystems. So anything
# ExternalProject clones or compiles is thrown away and done again next time,
# and SDL is not small. Building it here, once, into a directory that survives
# means ordinary builds pay nothing.
#
# ⚠️ RE-RUN THIS ONLY WHEN THE PATCH CHANGES. Not per build, not per session.
#
# Output lands in ../sdl-webos-patched/, beside the repos rather than inside
# any of them -- it is a build artefact, not source.
set -e
cd "$(dirname "$0")/.."

FORK_URL="${SDL_FORK_URL:-git@github.com:rhoquinn8217/SDL-webOS.git}"
FORK_TAG="${SDL_FORK_TAG:-release-2.30.12-webos.5-micflag}"
OUT_DIR="$(cd .. && pwd)/sdl-webos-patched"

echo "=== building patched SDL ==="
echo "  from: ${FORK_URL} @ ${FORK_TAG}"
echo "  into: ${OUT_DIR}"
echo

# The same container and toolchain the app uses. Building SDL with a different
# compiler than the app links it into is a class of problem worth not having.
docker run --rm --platform linux/amd64 \
  --dns 8.8.8.8 --dns 1.1.1.1 \
  -v "$(pwd):/build" \
  -v "$(pwd)/../.build-cache/tmp:/tmp" \
  -v "${OUT_DIR}:/sdl-out" \
  -w /build -e CI=1 \
  -e SDL_FORK_URL="${FORK_URL}" \
  -e SDL_FORK_TAG="${FORK_TAG}" \
  ubuntu:22.04 \
  bash -c '
set -e
apt-get update -qq
apt-get install -y -qq cmake git build-essential ca-certificates wget file

# The toolchain the app build downloads. Reuse it if it is already unpacked in
# the mounted tree, so this does not fetch it a second time.
# The same cached toolchain a normal build uses -- /tmp is mounted from
# ../.build-cache/tmp, so it is already unpacked and does not get downloaded
# again here.
SDK_ROOT=/tmp/arm-webos-linux-gnueabi_sdk-buildroot
if [ ! -x "${SDK_ROOT}/bin/arm-webos-linux-gnueabi-gcc" ]; then
  echo "!! toolchain not found at ${SDK_ROOT}"
  echo "   run a normal app build first so it is unpacked into the cache"
  exit 1
fi
export TOOLCHAIN_FILE="${SDK_ROOT}/share/buildroot/toolchainfile.cmake"
echo "toolchain: ${TOOLCHAIN_FILE}"

rm -rf /sdl-work && mkdir -p /sdl-work
git clone --quiet --depth 1 --branch "${SDL_FORK_TAG}" "${SDL_FORK_URL}" /sdl-work/src

# ⚠️ Proves the patch is actually in what we are about to build. A fork whose
# tag quietly points at unpatched code would produce an SDL that looks right
# and still lets the storm through.
if ! grep -q "data\[1\] & 0x01" /sdl-work/src/src/joystick/hidapi/SDL_hidapi_ps5.c; then
  echo "!! the microphone flag check is NOT in this source -- wrong tag?"
  exit 1
fi
echo "patch present: the flag check is in SDL_hidapi_ps5.c"

cmake -S /sdl-work/src -B /sdl-work/build \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=/sdl-out \
  -DWEBOS=ON -DSDL_OFFSCREEN=OFF -DSDL_DISKAUDIO=OFF \
  -DSDL_DUMMYAUDIO=OFF -DSDL_DUMMYVIDEO=OFF -DSDL_KMSDRM=OFF \
  -DSDL_VENDOR_INFO="webOS Backport"

cmake --build /sdl-work/build --parallel
cmake --install /sdl-work/build
'

echo
if [ -f "${OUT_DIR}/lib/libSDL2-2.0.so.0" ]; then
  echo "=== done ==="
  ls -la "${OUT_DIR}/lib/libSDL2-2.0.so.0"
  echo
  echo "Point the app at it by setting, in aurora-tv/CMakeLists.txt:"
  echo "    set(SDL2_BACKPORT_PREBUILT_DIR \"${OUT_DIR}\")"
else
  echo "!! nothing was produced -- check the output above"
  exit 1
fi
