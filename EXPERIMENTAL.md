# EXPERIMENTAL — Bluetooth microphone capture

**This branch is `mic-capture-experimental`. It is not the stable branch and it
is not meant to be merged into one.**

⛔ **It exists to do one thing the stable branch deliberately cannot: tell a
DualSense to stream its microphone over Bluetooth.**

---

## ⛔⛔ READ THIS BEFORE YOU ARM ANYTHING

A DualSense told to stream its microphone sends that audio in reports that look
**exactly** like button presses to anything reading them — same report id
(`0x31`), same length (78 bytes). **One bit in byte 1 distinguishes them, and
almost nothing checks it.**

**This branch patches SDL so Aurora checks it.** ⛔ **It cannot patch webOS.**
The TV reads the controller too, through the kernel's `hid-playstation` driver,
which has the same omission — and that kernel is LG's, signed, and not
replaceable.

| | |
|---|---|
| ✅ **Aurora open and in front** | **Protected.** Aurora holds the controller and ignores audio reports |
| ⛔ **Aurora gone** — crashed, switched away from, closed, mid-deploy | **The TV takes the controller back and cannot ignore them.** Measured on a C3: apps launching at random until the controller was powered off |

### ⭐⭐ START THE WATCHDOG FIRST — BEFORE AURORA, NOT ALONGSIDE IT

```
sh scripts/bt-capture/bt_cap_watchdog.sh &
```

⛔ **It is not an accompaniment to a capture session. It is the first thing that
should be running and the last thing that should stop.**

⭐ **The hazard it exists for is a controller armed by something that is NOT
this app** — a state left behind by an earlier session, another program, or
someone experimenting with the controller directly. ⚠️ **That hazard is present
BEFORE Aurora starts and AFTER it dies**, which is exactly when nothing else is
watching.

➡️ **Start it, leave it running, and let it outlive everything else on the TV.**

⚠️ **It is a net, not a guarantee** — up to a second of storm before it fires.
⛔ **The reliable stop is the controller's power button, or the TV's remote.**

⚠️ **Older scripts elsewhere say "run with Aurora closed". That advice predates
the SDL patch and is now exactly backwards.**

---

## 🔨 Building this branch

⛔ **A normal build will FAIL until you build the patched SDL first.**
`CMakeLists.txt` on this branch points at a prebuilt SDL and errors when it is
absent. **That is deliberate — an unpatched build of this branch is not safe to
run.**

**1. Build the patched SDL, once:**

```
./scripts/build-sdl-fork.sh
```

It builds `rhoquinn8217/SDL-webOS` at tag `release-2.30.12-webos.5-micflag` —
the webOS backport plus one guard, **eleven lines**, making SDL's PlayStation
driver check the audio flag. Output lands in `../sdl-webos-patched`, beside
this checkout.

⭐ **Once only.** It is rebuilt only when the patch changes. **Building it as
part of every app build costs ~500 seconds each time**, because the inner build
script wipes its build directory every run.

**2. Make it visible to the app build at `/sdl-patched`.**

⚠️ **This is a CONTAINER path, and it is the one thing this repository cannot
do for you.** `CMakeLists.txt` sets `SDL2_BACKPORT_PREBUILT_DIR` to
`/sdl-patched`; something has to put the library there.

✅ **`scripts/bt-capture/build-ipk-experimental.sh` does this for you.** It
mounts `../sdl-webos-patched` there, and **refuses to build if that folder is
missing** rather than quietly producing a package without the guard.

**3. Build with this branch's own script:**

```
./scripts/bt-capture/build-ipk-experimental.sh
```

⛔ **NOT `scripts/build-ipk.sh`.** That one is stable's and is inherited here by
every merge forward. **It does not mount the patched SDL**, so building with it
on this branch produces a package with no guard in it.

⭐ **How to tell it worked: the package size.** With the patched SDL linked in
it is around **4.05 MB**. Without it, around **2.18 MB** — which means you have
built the stable configuration by accident and **the guard is not in your
build.**

---

## ⛔⛔ MERGING THE STABLE BRANCH INTO THIS ONE

**Stable's job is removing what this branch exists to keep.** Every merge
forward will conflict, **on purpose.**

**You will see this, and it is the correct outcome:**

```
CONFLICT (modify/delete): cmake/ExternalSDL2BackportForWebOS.cmake
    deleted in <stable> and modified in HEAD.
CONFLICT (modify/delete): scripts/build-sdl-fork.sh
    deleted in <stable> and modified in HEAD.
```

➡️ **THE ANSWER IS ALWAYS: KEEP OURS.**

```
git checkout --ours cmake/ExternalSDL2BackportForWebOS.cmake
git checkout --ours scripts/build-sdl-fork.sh
git add cmake/ExternalSDL2BackportForWebOS.cmake scripts/build-sdl-fork.sh
```

⭐⭐ **WHY THE CONFLICT IS ENGINEERED.** Git only stops for a modify/delete
conflict when **both** sides touched the file. **If this branch had left those
files alone, stable's deletion would merge SILENTLY** — no warning, no
resolution step — and the branch would quietly lose the thing it exists to
hold. **So both files carry a banner comment purely so that every merge has to
stop and ask.**

⚠️ **A modify/delete conflict puts NO markers inside the file.** Nothing forces
you to open it. **That is why this document exists.**

⛔ **If you ever delete those banner comments, the protection goes with them.**

**Also expect `CMakeLists.txt` to conflict** in the SDL block. **Keep this
branch's version** — the `set(SDL2_BACKPORT_PREBUILT_DIR "/sdl-patched")` line.
Stable's version sets `SDL2_BACKPORT_RELEASE` instead and builds the stock
backport.

**After any merge, check the guard survived:**

```
grep -c sdl-patched CMakeLists.txt
ls cmake/ExternalSDL2BackportForWebOS.cmake scripts/build-sdl-fork.sh
```

⭐ **`1` and both files present.** Anything else means the merge took stable's
side.

---

## 📋 What is here, and what is not

**Here:** the SDL fork machinery, the arming code in the bridge core, and
`scripts/bt-capture/bt_cap_watchdog.sh` — **self-contained on purpose**, no
other file needed.

⛔ **NOT here:** the development and measurement scripts — arming by hand,
capture, sampling, the re-arming experiment. **They are kept outside the
repository deliberately**, because they can arm a controller and this branch
should ship only the parts that make it safe, not the parts that make it
dangerous.

---

## ⚠️ Current state of the feature

**It captures.** Audio has been pulled off a controller over Bluetooth, decoded
through libopus, and heard.

**Measured 2026-08-15**, three captures on a rooted monitor: **96%, 97% and
99.7% frame delivery**, every frame CRC-clean, **zero decode errors across 834
audio frames.** ⭐ **Decoded and listened to, the controller's own audio is
fine** — no distortion.

⛔ **The "underwater" quality heard through Windows is therefore NOT the
controller.** It is introduced downstream — in the TV-side capture handling or
the path to Windows. ⚠️ **Sample rate is the first suspect: the packets are
CELT at 10 ms and decode at 48 kHz.**

**The format, for anyone working on it:**

| Byte(s) | Meaning |
|---|---|
| 0 | report id, always `0x31` |
| 1 | high nibble a rolling counter; **bit 1 = audio, bit 0 = pad state** |
| 2 | audio sequence counter, +1 per audio frame — **how loss is measured** |
| 3 | Opus TOC, always `0xd4`: CELT, 10 ms, one frame per packet |
| 3–73 | the Opus packet, 71 bytes |
| 74–77 | CRC32, little-endian, over `0xA1` + bytes 0–73 |

⭐ **Both flags are never set at once.** That is what makes a one-bit check
sufficient.
