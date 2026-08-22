#!/bin/sh
# ⭐⭐ MERGE GUARD — run this immediately after merging GuiDev1994, before building.
#
# ⛔ WHAT IT EXISTS TO CATCH, and it nearly happened on 2026-08-21: his v1.2.2
# commit DELETED our USB Bridge pane registration from settings.controller.c.
# Git happened to keep our side. It could as easily have taken his, and the
# build would have compiled cleanly with the entire settings section gone.
#
# ⚠️ A MERGE THAT DROPS OUR CODE DOES NOT FAIL. It compiles, it runs, and the
# feature is simply absent. That is the failure mode this catches -- nothing
# here tests behaviour, and nothing here would have caught the v1.2.2 crash.
#
# ⓘ Grep rather than the C harness on purpose: no build, no toolchain, one
# second, and it can run on a tree that does not compile yet.
#
# Usage:  ./tests/merge-guard.sh
# Exit:   0 all present, 1 something was lost

fail=0
pass=0

need() {           # need <description> <file> <pattern>
    if grep -q "$3" "$2" 2>/dev/null; then
        pass=$((pass + 1))
    else
        echo "⛔ LOST: $1"
        echo "   expected in $2 : $3"
        fail=$((fail + 1))
    fi
}

absent() {         # absent <description> <file> <pattern>
    if grep -q "$3" "$2" 2>/dev/null; then
        echo "⛔ PRESENT AND SHOULD NOT BE: $1"
        echo "   found in $2 : $3"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
}

# --- the settings section ------------------------------------------------
# ⛔ This exact line was deleted by his v1.2.2. Everything else in the section
# can survive a merge and still be unreachable if this one goes.
need "USB Bridge pane registered" \
     src/app/ui/settings/settings.controller.c "settings_pane_usbbridge_cls"
need "USB Bridge pane declared" \
     src/app/ui/settings/settings.controller.h "settings_pane_usbbridge_cls"
need "USB Bridge pane compiled" \
     src/app/ui/settings/panes/CMakeLists.txt "usbbridge.pane.c"

# --- the settings themselves ---------------------------------------------
# ⓘ Each is read somewhere that would silently do nothing if the field vanished.
for s in bridge_enable bridge_gesture bridge_signal_light bridge_signal_rumble \
         bridge_signal_tone bridge_mic_wired bridge_mic_bt; do
    need "setting $s" src/app/app_settings.h "$s"
    need "setting $s persisted" src/app/app_settings.c "$s"
done

# --- the per-controller exclusion ----------------------------------------
# ⚠️ HIS FILE, AND HE REWROTE THIS FUNCTION IN v1.2.2. Without the mask a
# bridged controller keeps sending to Moonlight as well as to the bridge, and
# the game sees every input twice.
need "bridged controllers excluded from Moonlight" \
     src/app/stream/input/session_gamepad.c "moonlightExcludedMask"

# --- the overlay input hold ----------------------------------------------
# ⓘ Took three attempts to get right; the condition is easy to lose in a merge
# because it sits inside a function of his that he also edits.
need "input hold driven by the real overlay state" \
     src/app/app.c "streaming_overlay_shown()"
need "input hold handed to the core" \
     src/app/ctmbridge/ctm_bridge_glue.c "ctm_bridge_set_input_held"

# --- the branch switch ---------------------------------------------------
# ⛔ THE ONE LINE THAT SEPARATES THE BRANCHES. On stable it must be absent, so
# every gated block compiles out. On mic-capture-experimental it must be 1.
# ⚠️ BY CONTENT, NOT BY BRANCH NAME. A working copy checked out under any other
# name -- a bisect branch, a local experiment -- would otherwise be judged
# against the wrong rules and report a loss that is not one.
if grep -q "CTM_BT_MIC_ARMING=1" CMakeLists.txt 2>/dev/null; then
    branch=mic-capture-experimental
else
    branch=stable
fi
case "$branch" in
    mic-capture-experimental)
        need "experimental arms the BT microphone" \
             CMakeLists.txt "CTM_BT_MIC_ARMING=1"
        need "experimental marks its packages" \
             CMakeLists.txt 'CTM_BUILD_SUFFIX "_EXP"'
        ;;
    *)
        absent "stable must NOT define CTM_BT_MIC_ARMING" \
               CMakeLists.txt "add_compile_definitions(CTM_BT_MIC_ARMING"
        ;;
esac

# --- the panel -----------------------------------------------------------
need "USB Bridge panel present" \
     src/app/ui/streaming/ctm_panel.c "ctm_panel_open"
need "panel offers the overlay button" \
     src/app/ui/streaming/streaming.view.c "bridge_enable"

echo
if [ "$fail" -eq 0 ]; then
    echo "⭐ merge guard: $pass checks, nothing lost"
    exit 0
fi
echo "⛔ merge guard: $fail LOST, $pass intact"
echo "   ⚠️ A merge dropped our code. Do NOT build and test around it --"
echo "      find what took the other side and put it back."
exit 1
