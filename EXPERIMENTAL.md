# EXPERIMENTAL — Bluetooth microphone capture

**Branch: `mic-capture-experimental`.** It is not the stable branch and it is
never merged back into one.

⛔ **It does one thing the stable branch deliberately cannot: tell a DualSense
to stream its microphone over Bluetooth.**

---

## ⛔⛔ SAFETY — READ BEFORE YOU ARM ANYTHING

**An armed DualSense sends audio in reports that look exactly like button
presses to anything reading them** — same report id, same length, one bit
apart. **This branch patches SDL so Aurora checks that bit. It cannot patch
webOS**, which reads the controller too and has the same omission.

| | |
|---|---|
| ✅ **Aurora open and in front** | **Protected.** |
| ⛔ **Aurora gone** — crashed, switched away from, closed, mid-deploy | **The TV takes the controller back and cannot ignore the audio.** Observed: apps launching at random until the controller was powered off. |

### ⭐⭐ Start the watchdog FIRST — before Aurora, not alongside it

```
sh scripts/bt-capture/bt_cap_watchdog.sh &
```

**It silences any controller it finds streaming.** ⭐ **Run it before anything
else and leave it running** — the hazard is a controller armed by something
that is not this app, which means before Aurora starts and after it dies.

⚠️ **It is a net, not a guarantee** — up to a second before it fires.
⛔ **The reliable stop is the controller's power button, or the TV's remote.**

⚠️ **Older scripts elsewhere say "run with Aurora closed". That is out of date
and now backwards.**

---

## 🔨 Building

⛔ **A build will fail until the patched SDL exists. That is deliberate — an
unpatched build of this branch is not safe to run.**

**1. Build the patched SDL, once:**

```
./scripts/build-sdl-fork.sh
```

Output lands in `../sdl-webos-patched`, beside this checkout. ⭐ **Rebuild only
when the patch changes** — it is slow, and the app build does not need it
repeated.

**2. Build the app with this branch's own script:**

```
./scripts/bt-capture/build-ipk-experimental.sh
```

⛔ **NOT `scripts/build-ipk.sh`.** That one is stable's, inherited here by every
merge forward, and **does not mount the patched SDL** — building with it
produces a package with no guard in it.

⭐ **The package size tells you which SDL went in:** roughly **4 MB** with the
patch, roughly **2 MB** without. **The smaller size on this branch means the
guard is missing.**

⭐ **So does the app itself:** the Info tab reads `1.1.7 (N_EXP)` here. **A
build without the suffix is not this branch.**

---

## ⛔⛔ MERGING STABLE INTO THIS BRANCH

**Stable removes what this branch keeps. Every merge forward needs resolving —
a clean merge means something went wrong quietly.**

### Two files will conflict. Keep ours.

```
git checkout --ours cmake/ExternalSDL2BackportForWebOS.cmake
git checkout --ours scripts/build-sdl-fork.sh
git add cmake/ExternalSDL2BackportForWebOS.cmake scripts/build-sdl-fork.sh
```

⚠️ **Both carry a banner comment for a reason: without a change on this side,
stable's deletion would merge silently.** ⛔ **Delete those comments and the
protection goes with them.**

### ⛔ `CMakeLists.txt` will NOT conflict, and that is the dangerous part

**Git takes stable's version without reporting anything**, removing the line
that points the build at the patched SDL. ⛔ **Resolve the two conflicts above,
commit, and you have a branch that builds, runs, and has no guard in it.**

```
git checkout HEAD -- CMakeLists.txt
```

⚠️ **That takes this branch's whole file — check the diff for anything else
stable changed in it before accepting.**

### ⛔ Then verify. Not optional.

```
grep -c sdl-patched CMakeLists.txt
ls cmake/ExternalSDL2BackportForWebOS.cmake scripts/build-sdl-fork.sh
```

⭐ **`1`, and both files present.** Anything else means the merge took stable's
side.

⚠️ **A conflict that fires is not proof the merge was safe. The files that do
NOT conflict are the danger.**

---

## 📋 What is here, and what is not

**Here:** the SDL fork machinery, the arming code in the bridge core, and
`scripts/bt-capture/bt_cap_watchdog.sh` — **self-contained, no other file
needed, and it can only ever silence a controller.**

⛔ **NOT here:** the development and measurement scripts — arming by hand,
capture, sampling. **Deliberately kept out**, because they can arm a controller
and this branch ships only the parts that make it safe.

⚠️ **The bridge core is a sibling checkout, not a submodule.** Clone
`ctm-bridge-webos` next to this repository and **check out the matching branch
there too** — the arming code lives in the core, not here.

---

## ⚠️ Status

**Capture works. The audio quality does not.** Recording through the virtual
microphone on Windows sounds slowed and thickened. **Cause not established.**

⛔ **This branch is not a finished feature. Treat it as a work in progress with
a hazard attached.**
