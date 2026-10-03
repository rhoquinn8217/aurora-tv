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

need_count() {     # need_count <description> <file> <pattern> <lines>
    n=$(grep -c "$3" "$2" 2>/dev/null)
    if [ "${n:-0}" = "$4" ]; then
        pass=$((pass + 1))
    else
        echo "⛔ LOST: $1"
        echo "   expected $4 lines in $2 matching : $3 (found ${n:-0})"
        fail=$((fail + 1))
    fi
}

# The two below read one function: from the line that begins with <start> to
# the first line that begins with "}". Texts are matched as they are written,
# not as patterns. A function that is not there counts as lost.
in_order() {       # in_order <description> <file> <start> <first text> <second text>
    if awk -v s="$3" -v a="$4" -v b="$5" '
        index($0, s) == 1 { inside = 1 }
        inside && !ia && index($0, a) { ia = NR }
        inside && !ib && index($0, b) { ib = NR }
        inside && /^}/ { inside = 0 }
        END { exit !(ia && ib && ia < ib) }' "$2" 2>/dev/null; then
        pass=$((pass + 1))
    else
        echo "⛔ LOST: $1"
        echo "   expected in $2, inside $3 : \"$4\" before \"$5\""
        fail=$((fail + 1))
    fi
}

absent_in_function() {   # absent_in_function <description> <file> <start> <text>
    if awk -v s="$3" -v t="$4" '
        index($0, s) == 1 { inside = 1; seen = 1 }
        inside && index($0, t) { hit = 1 }
        inside && /^}/ { inside = 0 }
        END { exit !(seen && !hit) }' "$2" 2>/dev/null; then
        pass=$((pass + 1))
    else
        echo "⛔ LOST: $1"
        echo "   expected in $2, inside $3, no \"$4\" (or the function is gone)"
        fail=$((fail + 1))
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
     src/app/ctmbridge/app_sources.cmake "usbbridge.pane.c"

# --- the fork's build files ----------------------------------------------
# ⛔ EVERYTHING OF OURS HANGS OFF ONE LINE IN A FILE OF UPSTREAM'S. If a merge
# takes upstream's side of src/app/CMakeLists.txt, nothing of ours is compiled.
# ⓘ That fails the build loudly, which the other cases in this file do not; the
# check is here because it names the cause in one second.
need "the bridge's build file is still called" \
     src/app/CMakeLists.txt "add_subdirectory(ctmbridge)"
need "our source list is read" \
     src/app/ctmbridge/CMakeLists.txt "app_sources.cmake"
need "the build number's file is read" \
     src/app/ctmbridge/CMakeLists.txt "build_number.cmake"

# ⓘ The build number lives in a file of ours and nowhere else. On the line under
# upstream's version it conflicted with every release that was merged (13 of
# 13), so a merge that brings it back there has undone the move.
need "the build number is in our file" \
     src/app/ctmbridge/build_number.cmake "^set(CTM_BUILD_NUMBER [0-9][0-9]*)"
absent "the build number is NOT back in upstream's build file" \
       CMakeLists.txt "CTM_BUILD_NUMBER"

# Every source file our list names is in the tree.
for f in $(grep -o 'src/app/[A-Za-z0-9_./]*\.c' src/app/ctmbridge/app_sources.cmake 2>/dev/null); do
    if [ -f "$f" ]; then
        pass=$((pass + 1))
    else
        echo "⛔ LOST: $f"
        echo "   named in src/app/ctmbridge/app_sources.cmake and not in the tree"
        fail=$((fail + 1))
    fi
done

# ⚠️ A FILE THAT SHOWS A VERSION MUST BE NAMED IN build_number.cmake, which gives
# it the header that carries the build number. One that upstream adds, printing
# APP_VERSION, would compile and show the version WITHOUT the build number.
for f in $(grep -rl "APP_VERSION" src/app --include='*.c' 2>/dev/null); do
    need "the build number reaches the version $f shows" \
         src/app/ctmbridge/build_number.cmake "$f"
done

# --- the bridge core -----------------------------------------------------
# ⭐ THE CORE IS A SUBMODULE, third_party/ctm-bridge-webos, and every commit here
# records exactly which commit of it is built. It used to be read from the folder
# beside the checkout, which a clone does not have and which built whatever it
# held, committed or not. A merge that took upstream's .gitmodules, or brought
# the old path back into the bridge's build file, would undo that.
need "the core is declared as a submodule" \
     .gitmodules "third_party/ctm-bridge-webos"
need "the build reads the core from inside the tree" \
     src/app/ctmbridge/CMakeLists.txt "CMAKE_SOURCE_DIR}/third_party/ctm-bridge-webos"
absent "the build does NOT read the folder beside the checkout" \
       src/app/ctmbridge/CMakeLists.txt "/\.\./ctm-bridge-webos"
if git ls-files -s third_party/ctm-bridge-webos 2>/dev/null | grep -q '^160000 '; then
    pass=$((pass + 1))
else
    echo "⛔ LOST: the core's commit is recorded"
    echo "   expected: third_party/ctm-bridge-webos as a submodule entry (git ls-files -s)"
    fail=$((fail + 1))
fi

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

# ⚠️ WE LEAN ON UPSTREAM'S OWN CODE HERE. The announce at stream start has no
# check of ours any more: session_input_started() is upstream's text, and an
# excluded controller is refused inside stream_input_send_gamepad_arrive(),
# which asks stream_input_gamepad_sends_moonlight(), where the mask is read.
# If a release stops the arrive from asking, a controller that is still
# bridged when a stream reconnects is announced again and the host has two.
if awk '/^void stream_input_send_gamepad_arrive/,/^}/' \
       src/app/stream/input/session_gamepad.c 2>/dev/null |
   grep -q "stream_input_gamepad_sends_moonlight"; then
    pass=$((pass + 1))
else
    echo "⛔ LOST: the announce asks whether a controller is excluded"
    echo "   expected inside stream_input_send_gamepad_arrive() in"
    echo "   src/app/stream/input/session_gamepad.c : stream_input_gamepad_sends_moonlight"
    fail=$((fail + 1))
fi

# --- the overlay input hold ----------------------------------------------
# ⓘ Took three attempts to get right. The condition lives in bridge_app.c now,
# and app.c, upstream's file, keeps the one call that runs it.
# ⚠️ Matched on the assignment, not on the call alone: the comment above it
# names streaming_overlay_shown() too, and a check on the bare name passed
# with the code itself broken (found 2026-10-02 by breaking it on purpose).
need "input hold driven by the real overlay state" \
     src/app/bridge_app.c "interface_has_input = streaming_overlay_shown()"
need "input hold handed to the core" \
     src/app/ctmbridge/ctm_bridge_glue.c "ctm_bridge_set_input_held"

# --- the calls app.c makes into bridge_app.c --------------------------------
# ⓘ app.c is upstream's application file and edited in most releases. The
# fork's code there is five one-line calls into src/app/bridge_app.c, and a
# merge that takes upstream's side of app.c drops them without a sound.
need "app.c disarms a microphone left streaming, at start" \
     src/app/app.c "bridge_app_before_sdl();"
in_order "... and does it before SDL_Init" \
     src/app/app.c "int app_init" "bridge_app_before_sdl();" "SDL_Init(0);"
need "app.c asks PlayStation controllers for their full report" \
     src/app/app.c "bridge_app_sdl_hints();"
need "app.c starts the command port" \
     src/app/app.c "bridge_app_started(app);"
need "app.c stops the command port" \
     src/app/app.c "bridge_app_stopping();"
in_order "app.c runs the bridge's turn after SDL's events are filtered" \
     src/app/app.c "void app_process_events" "SDL_FilterEvents(app_event_filter, app);" "bridge_app_events(app);"

# --- the calls the stream files make into files of ours ------------------------
# ⓘ session.c and session_events.c are upstream's stream files. The bridge's
# start and stop, its hold on the virtual mouse and the remote's pointer are a
# call each into src/app/stream/bridge_session.c and bridge_pointer.c, and the
# order of each call against upstream's own line beside it is what makes it
# work.
need "session.c lets Bridge Override keep the virtual mouse off at stream start" \
     src/app/stream/session.c "session->config.vmouse && bridge_session_vmouse_allowed()"
in_order "session.c starts the bridge after the input has started" \
     src/app/stream/session.c "bool session_start_input" \
     "session_input_started(&session->input);" "bridge_session_started(session);"
in_order "session.c stops the bridge after the input has stopped" \
     src/app/stream/session.c "void session_stop_input" \
     "session_input_stopped(&session->input);" "bridge_session_stopped();"
need "session.c lets Bridge Override hold the virtual mouse off on a toggle" \
     src/app/stream/session.c "if (bridge_session_hold_vmouse_off(session)) {"
in_order "the remote's pointer takes its events before the stream's own input" \
     src/app/stream/session_events.c "bool session_handle_input_event" \
     "bridge_pointer_event(session, event)" "switch (event->type)"

# --- the branch switch ---------------------------------------------------
# ⛔ THE ONE LINE THAT SEPARATES THE BRANCHES. On stable it must be absent, so
# every gated block compiles out. On mic-capture-experimental it must be 1.
# ⚠️ BY CONTENT, NOT BY BRANCH NAME. A working copy checked out under any other
# name -- a bisect branch, a local experiment -- would otherwise be judged
# against the wrong rules and report a loss that is not one.
# ⓘ The switch may sit in any of the fork's build files: on a branch cut before
# the build number moved, it and the suffix are still in the root CMakeLists.txt.
BUILD_FILES="CMakeLists.txt src/app/ctmbridge/CMakeLists.txt src/app/ctmbridge/app_sources.cmake src/app/ctmbridge/build_number.cmake"

build_files_have() {   # build_files_have <pattern>
    cat $BUILD_FILES 2>/dev/null | grep -q "$1"
}

need_built() {     # need_built <description> <pattern>
    if build_files_have "$2"; then
        pass=$((pass + 1))
    else
        echo "⛔ LOST: $1"
        echo "   expected in one of the build files ($BUILD_FILES) : $2"
        fail=$((fail + 1))
    fi
}

absent_built() {   # absent_built <description> <pattern>
    if build_files_have "$2"; then
        echo "⛔ PRESENT AND SHOULD NOT BE: $1"
        echo "   found in one of the build files ($BUILD_FILES) : $2"
        fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
}

if build_files_have "CTM_BT_MIC_ARMING=1"; then
    branch=mic-capture-experimental
else
    branch=stable
fi
case "$branch" in
    mic-capture-experimental)
        need_built "experimental arms the BT microphone" "CTM_BT_MIC_ARMING=1"
        need_built "experimental marks its packages" 'CTM_BUILD_SUFFIX "_EXP"'
        ;;
    *)
        absent_built "stable must NOT define CTM_BT_MIC_ARMING" \
                     "add_compile_definitions(CTM_BT_MIC_ARMING"
        ;;
esac

# --- the places ctmbridge/NOTES.md explains ------------------------------
# ⓘ Each change below was explained by a long comment in a file of upstream's.
# The explanation is in src/app/ctmbridge/NOTES.md now, one line at the place
# points to it, and these checks take over the warnings it carried.
need "a pad already opened is not opened again" \
     src/app/input/input_event.c "app_input_gamepad_state_by_instance_id(input, joy_instance_id) != NULL"
need "the host's lightbar waits while a bridge signal draws" \
     src/app/input/input_gamepad.c "ctm_bridge_gesture_light_busy(state->controller)"
need_count "upstream's wired feedback stays off a pad the bridge is using, at all four entry points" \
     src/app/input/input_gamepad.c "ctm_bridge_gesture_pad_is_ours(state->controller)" 4
need "Select and Start are held back while both bumpers are down" \
     src/app/stream/input/session_gamepad.c "CHORD_GATE_HELD) == CHORD_GATE_HELD"
in_order "a controller is removed from the host before its bit is set" \
     src/app/stream/input/session_input.c "void stream_input_exclude_gamepad" \
     "stream_input_send_gamepad_remove(input, gamepad)" "moonlightExcludedMask |="
in_order "its bit is cleared before it is announced again" \
     src/app/stream/input/session_input.c "void stream_input_restore_gamepad" \
     "moonlightExcludedMask &=" "stream_input_send_gamepad_arrive(input, gamepad)"
absent_in_function "the remove path does not consult the mask" \
     src/app/stream/input/session_gamepad.c "void stream_input_send_gamepad_remove" "moonlightExcludedMask"
need "the overlay's shortcut acts at once" \
     src/app/stream/input/session_keyboard.c "_pending_key_combo == KeyComboToggleStatsOverlay"
need "a stream that drops resumes in place" \
     src/app/stream/session_worker.c "interrupt_reason == STREAMING_INTERRUPT_NETWORK"
need "the stream boost is a setting" \
     src/app/ui/settings/panes/experimental.pane.c "app_configuration->stream_priority"
need "the stream boost setting is read before the connection starts" \
     src/app/stream/session_worker.c "settings.stream_priority"

# ⓘ The notes and the code stay in step: every "This fork:" line names a section
# that exists, and every section is named by at least one such line.
NOTES=src/app/ctmbridge/NOTES.md
named=$(grep -rho --exclude=NOTES.md 'ctmbridge/NOTES\.md, "[^"]*"' src 2>/dev/null | sed 's/^.*, "//; s/"$//' | sort -u)
written=$(tr -d '\r' < "$NOTES" 2>/dev/null | sed -n 's/^## //p' | sort -u)
if [ -n "$named" ] && [ "$named" = "$written" ]; then
    pass=$((pass + 1))
else
    echo "⛔ LOST: the one-line notes and $NOTES name the same sections"
    echo "   named in the code but not a section there:"
    printf '%s\n' "$named" | while IFS= read -r s; do
        printf '%s\n' "$written" | grep -qxF "$s" || echo "     $s"
    done
    echo "   a section there that no line of code names:"
    printf '%s\n' "$written" | while IFS= read -r s; do
        printf '%s\n' "$named" | grep -qxF "$s" || echo "     $s"
    done
    fail=$((fail + 1))
fi

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
