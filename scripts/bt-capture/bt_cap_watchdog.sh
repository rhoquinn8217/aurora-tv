#!/bin/sh
# ⛔⛔ THE SAFETY NET FOR BLUETOOTH MICROPHONE CAPTURE. Runs on the TV.
#
# ⭐ SELF-CONTAINED ON PURPOSE. It depends on no other script and no other
# file. Everything it needs -- including the packet that silences a controller
# -- is inside it. Copy this one file to the TV and it works.
#
# ═══════════════════════════════════════════════════════════════════════════
# WHY THIS EXISTS
# ═══════════════════════════════════════════════════════════════════════════
#
# A DualSense told to stream its microphone sends that audio in reports that
# look EXACTLY like button presses to anything that reads them: same report id
# (0x31), same length (78 bytes). One bit in byte 1 says which kind it is, and
# almost nothing checks that bit.
#
# Aurora's own copy of SDL is patched on this branch and ignores them. ⛔ But
# webOS ITSELF also reads the controller, through the kernel's hid-playstation
# driver, which has the same omission and which we cannot patch -- LG's kernel
# is signed and not ours to replace.
#
# ⚠️ Measured on a C3, 2026-08-14: with the app not in front, apps launched at
# random until the controller was powered off.
#
# ═══════════════════════════════════════════════════════════════════════════
# ⭐⭐ WHEN YOU ARE AT RISK, AND WHEN YOU ARE NOT
# ═══════════════════════════════════════════════════════════════════════════
#
# ✅ AURORA OPEN AND IN FRONT: you are protected. Aurora holds the controller
#    and its patched SDL ignores audio reports.
#
# ⛔ AURORA GONE -- crashed, switched away from, closed, or mid-deploy: the TV
#    takes the controller back, and the TV cannot ignore them.
#
# ⚠️ NOTE FOR ANYONE READING OLDER SCRIPTS: several of them say "RUN WITH
# AURORA CLOSED". That advice PREDATES the SDL fix and is now exactly
# backwards. Closing Aurora is what exposes you.
#
# ═══════════════════════════════════════════════════════════════════════════
# WHAT IT DOES, AND WHAT IT DOES NOT
# ═══════════════════════════════════════════════════════════════════════════
#
# Reads a few reports from each controller every second. If any carries audio,
# it sends the silence packet -- five times, because a single write can be lost
# and the cost of an extra one is nothing.
#
# ⚠️ IT IS NOT INSTANT AND IT PREVENTS NOTHING. Up to about a second of storm
# before it fires, which is enough to activate something. ⭐ IT IS A NET, NOT A
# GUARANTEE. The reliable stop is still the controller's power button, or the
# TV's remote.
#
# ═══════════════════════════════════════════════════════════════════════════
# USAGE
# ═══════════════════════════════════════════════════════════════════════════
#
#   sh bt_cap_watchdog.sh                    watch every controller, forever
#   sh bt_cap_watchdog.sh /dev/hidraw0       watch one
#   sh bt_cap_watchdog.sh --off /dev/hidraw0 silence one, once, and exit
#   sh bt_cap_watchdog.sh --off              silence everything, once, and exit
#
# ⭐⭐ RUN THIS FIRST -- BEFORE AURORA, NOT ALONGSIDE IT.
#
#   sh bt_cap_watchdog.sh &
#
# ⛔ It is not an accompaniment to a capture session. It is the first thing that
# should be running on the TV and the last thing that should stop.
#
# ⭐ The hazard it exists for is a controller armed by something that is NOT
# this app: a state left behind by an earlier session, another program, or
# someone experimenting with the controller directly. ⚠️ A controller keeps
# streaming after the program that asked it is gone, and forgets only when its
# Bluetooth link drops.
#
# ➡️ So the exposure is BEFORE Aurora starts and AFTER it dies -- precisely
# when nothing else is watching. Start this, leave it running, let it outlive
# everything else.
#
# ⚠️ Written for busybox: no `fold`, no `strtonum` in awk, nothing clever.

LOG="/tmp/ctm-mic-watchdog.log"
PKT="/tmp/bt_cap_off.bin"
NODES_DEFAULT="/dev/hidraw0 /dev/hidraw1 /dev/hidraw2 /dev/hidraw3"

# ── The silence packet ──────────────────────────────────────────────────────
#
# Bluetooth output report 0x32, 142 bytes: one block (0x91), one payload byte,
# and the CRC32 every Bluetooth output report carries.
#
# ⭐ The payload byte is the whole point. Bit 0 is the microphone: 0b11 arms it,
# 0b10 silences it. This packet is the 0b10 one -- it can ONLY turn a
# microphone off. There is deliberately no arming packet in this file.
#
# ⚠️ It is a STATE CHANGE, not a stream. Send it once and the controller
# remembers. It forgets only when its Bluetooth link drops.
write_packet() {
    base64 -d > "$PKT" <<'B64EOF'
MhCRAQIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAsbaFnA==
B64EOF
    SIZE=$(wc -c < "$PKT" 2>/dev/null)
    if [ "$SIZE" != "142" ]; then
        echo "!! silence packet is $SIZE bytes, expected 142 -- refusing to send"
        exit 1
    fi
}

# ── Silence one node, repeatedly ────────────────────────────────────────────
silence() {
    k=0
    while [ $k -lt 5 ]; do
        dd if="$PKT" of="$1" bs=142 count=1 2>/dev/null
        k=$((k+1))
    done
}

# ── Is this node sending audio right now? ───────────────────────────────────
#
# Walks the reports: id 0x31 is 78 bytes, id 0x01 is 10. Bit 1 of byte 1 means
# the report carries audio. ⭐ One such report is proof -- this reads a flag the
# controller sets, it does not guess from odd stick values.
is_streaming() {
    dd if="$1" of=/tmp/bt_cap_probe.bin bs=78 count=12 2>/dev/null || return 1
    od -An -tx1 -v /tmp/bt_cap_probe.bin 2>/dev/null | tr -d ' \n' | awk '
      function hex2(s,   h,i,c,v) { h="0123456789abcdef"; v=0
        for (i=1;i<=2;i++){ c=index(h,substr(s,i,1))-1; v=v*16+c } return v }
      { n=length($0)/2; i=1
        while (i<=n) {
          id=hex2(substr($0,(i-1)*2+1,2))
          if (id==1) { i+=10; continue }
          if (id!=49) break
          f=hex2(substr($0,i*2+1,2))
          if (int(f/2)%2==1) { print "yes"; exit }
          i+=78
        }
        print "no" }'
}

write_packet

# ── One-shot silence, then exit ─────────────────────────────────────────────
if [ "$1" = "--off" ]; then
    shift
    for n in ${*:-$NODES_DEFAULT}; do
        [ -e "$n" ] || continue
        silence "$n"
        echo "silenced $n"
        echo "$(date) silenced $n on request" >> "$LOG"
    done
    exit 0
fi

# ── Watch ───────────────────────────────────────────────────────────────────
ONLY="$1"
echo "watchdog started $(date)" >> "$LOG"
echo "watching ${ONLY:-every controller} -- silences anything found streaming"
echo "⚠️  a net, not a guarantee: up to a second of storm before it fires"
echo "⛔ the reliable stop is the controller's power button"

while :; do
    for n in ${ONLY:-$NODES_DEFAULT}; do
        [ -e "$n" ] || continue
        if [ "$(is_streaming "$n")" = "yes" ]; then
            echo "⛔ $n is streaming microphone audio -- silencing"
            echo "$(date) $n streaming -- silencing" >> "$LOG"
            silence "$n"
        fi
    done
    sleep 1
done
