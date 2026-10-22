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
# ⓘ The ten bridge settings are one member of upstream's settings struct, and
# src/app/bridge_settings.c holds the rest. Upstream's settings code makes
# three calls into it, and losing any of them compiles: without the defaults
# every switch starts off and the auto-bridge list is unset, without the save
# a change is gone at the next start, and without the read every TV is back
# to the defaults.
need "the bridge's settings are a member of the app's" \
     src/app/app_settings.h "bridge_settings_t bridge;"
in_order "the bridge's defaults are set after the settings are cleared" \
     src/app/app_settings.c "void settings_initialize" \
     "memset(config, 0, sizeof(CONFIGURATION));" "bridge_settings_defaults(&config->bridge);"
in_order "the bridge's settings are saved while the file is open" \
     src/app/app_settings.c "bool settings_save" \
     'fopen(config->ini_path, "w");' "bridge_settings_write(fp, &config->bridge);"
in_order "the bridge's settings are saved before the file is closed" \
     src/app/app_settings.c "bool settings_save" \
     "bridge_settings_write(fp, &config->bridge);" "return fclose(fp) == 0;"
in_order "the bridge's settings are read with the rest" \
     src/app/app_settings.c "static int settings_parse" \
     "if (bridge_settings_parse(&config->bridge, name, value)) {" 'INI_FULL_MATCH("streaming", "width")'

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
# excluded is announced to the host as well, and the host has two.
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
need "a Virtual Mouse press switches Bridge Override off, in session.c" \
     src/app/stream/session.c "if (bridge_session_vmouse_pressed(session)) {"
in_order "the remote's pointer takes its events before the stream's own input" \
     src/app/stream/session_events.c "bool session_handle_input_event" \
     "bridge_pointer_event(session, event)" "switch (event->type)"

# --- the calls upstream's input and interface files make into files of ours ----
# ⓘ One line each, in five more of upstream's files, and a merge that takes his
# side of any of them compiles and runs with the behaviour quietly gone. They
# were missing from this file until the code review of 2026-10-05.
# ⛔ The TV's keyboard grab must never take a bridged keyboard: when it did,
# every key arrived twice (seen on the C1, 2026-09-30). input/bridge_keyboard.h
# has the whole story.
in_order "the keyboard grab leaves a bridged keyboard alone, before it grabs" \
     src/app/platform/webos/keyboard_evdev.c "static int open_keyboards(keyboard_evdev_t *kbd) {" \
     "if (bridge_keyboard_node_is_bridged(path)) {" "if (ioctl(fd, EVIOCGRAB, 1) < 0) {"
# ⛔ Nor webOS's own virtual devices (CHECK INPUT, IoT keypad and the like),
# which upstream's name list does not catch: input/bridge_keyboard.c says why.
in_order "the keyboard grab leaves webOS's own devices alone, before it grabs" \
     src/app/platform/webos/keyboard_evdev.c "static int open_keyboards(keyboard_evdev_t *kbd) {" \
     "if (bridge_keyboard_node_is_webos_own(fd, path)) {" "if (ioctl(fd, EVIOCGRAB, 1) < 0) {"
in_order "the keyboard grab records what it took" \
     src/app/platform/webos/keyboard_evdev.c "static int open_keyboards(keyboard_evdev_t *kbd) {" \
     "if (ioctl(fd, EVIOCGRAB, 1) < 0) {" "bridge_keyboard_took(path, held);"
# ⓘ Aurora's shortcuts and the overlay's turn with the keyboard live in that
# call: upstream's grab sends the keys straight to the host, around the place
# they used to be found.
in_order "a grabbed keyboard's keys are asked about before they go to the host" \
     src/app/stream/input/session_input.c "static void session_keyboard_evdev_cb" \
     "if (bridge_keyboard_evdev_key(input, vk, down, modifiers)) {" "stream_input_send_key_event(input, vk, down, modifiers);"
in_order "the interface drops a key the keyboard grab already fed it" \
     src/app/lvgl/input/lv_drv_sdl_key.c "static void sdl_input_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {" \
     "const int fed = bridge_keyboard_sdl_key(&e.key);" "webos_key_input_mode(input, &e.key);"
# ⓘ Bridge Override changes the stream's live copy of the settings, so it has
# to come after upstream fills that copy in, or upstream's line puts it back.
in_order "Bridge Override applies after the stream's settings are copied" \
     src/app/stream/input/session_input.c "void session_input_init" \
     "input->touchpad_scroll_scale = config->touchpad_natural_scroll" "bridge_override_apply(input);"
need "the bridge's question holds the input as the overlay does" \
     src/app/ui/root.c "streaming_soft_keyboard_shown() || bridge_prompt_shown()"
need "a removed controller's gesture is forgotten" \
     src/app/input/input_event.c "ctm_bridge_gesture_reset(event->jdevice.which);"

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
need "the pad scan's flush waits until SDL's walk of its event queue is over" \
     src/app/input/input_gamepad.c "ctm_bridge_gesture_flush_device_added();"
absent "the pad scan does NOT flush SDL's event queue itself (inside the walk, it tangles the queue)" \
     src/app/input/input_gamepad.c "SDL_FlushEvent(SDL_JOYDEVICEADDED)"
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

# --- what the control port says is on the screen (2026-10-17) -------------
# ⓘ Tests that drive the TV with nobody at it read the overlay, the panel and
# the rest through `ui`; losing the command or the panel's answer compiles.
need "the control port answers ui" \
     src/app/control_server.c 'strcasecmp(verb, "ui")'
need "the panel says whether it is open" \
     src/app/ui/streaming/ctm_panel.c "bool ctm_panel_shown(void)"
# ⓘ The overlay's USB Bridge and DS5-USBIP buttons are made by
# src/app/ui/streaming/bridge_overlay.c, called from upstream's view. Right
# after Virtual Mouse and before the spacer is what puts them third and fourth
# in the row and in the focus order.
in_order "the overlay's bridge buttons come after Virtual Mouse" \
     src/app/ui/streaming/streaming.view.c "lv_obj_t *streaming_scene_create" \
     'lv_label_set_text(vmouse_label, locstr("Virtual Mouse"));' "bridge_overlay_buttons_create(controller, actions);"
in_order "the overlay's bridge buttons come before the spacer" \
     src/app/ui/streaming/streaming.view.c "lv_obj_t *streaming_scene_create" \
     "bridge_overlay_buttons_create(controller, actions);" "lv_obj_t *actions_spacing = lv_obj_create(actions);"

# --- the pop-up a bridge raises, in upstream's streaming screen ----------------
# ⓘ Each is one line in streaming.controller.c, and losing any of them
# compiles: no question when a controller is bridged, a question left sitting
# on top of the overlay holding the input, or one outliving its stream.
in_order "a bridged controller raises the pop-up" \
     src/app/ui/streaming/streaming.controller.c "static bool on_event" \
     "case USER_CTM_MOUSE_MODE_WARN:" "bridge_prompt_request();"
in_order "the overlay sends the pop-up away as it opens" \
     src/app/ui/streaming/streaming.controller.c "bool show_overlay" \
     "bridge_prompt_dismiss();" "overlay_showing = true;"
in_order "the pop-up, its message and the TV cursor's align bar go with the stream" \
     src/app/ui/streaming/streaming.controller.c "static void controller_dtor" \
     "bridge_prompt_stream_ended();" "fragment->soft_kbd = NULL;"
in_order "the USB Bridge panel goes before the screen's focus group" \
     src/app/ui/streaming/streaming.controller.c "static void on_delete_obj" \
     "ctm_panel_on_owner_deleted(controller);" "lv_group_del(controller->group);"

# --- nothing held at a switch (rhoquinn8217, 2026-10-09) -------------------
# ⓘ A release switches controllers: what is held as a pad comes back from a
# bridge reaches the stream released until it is let go (bridge_switch_hold.c).
# Losing any of these compiles, and a button held at the release is held on
# the stream's controller.
need "the switch's file is built" \
     src/app/stream/input/CMakeLists.txt "bridge_switch_hold.c"
need_count "what the stream is sent leaves out what was held at the switch" \
     src/app/stream/input/session_gamepad.c "bridge_switch_hold_send(gamepad, input->input->activeGamepadMask" 2
need "a button held at the switch works again once let go" \
     src/app/stream/input/session_gamepad.c "bridge_switch_hold_note_button(gamepad, button"
need "a stick or trigger held at the switch works again once at rest" \
     src/app/stream/input/session_gamepad.c "bridge_switch_hold_note_axes(gamepad);"
need "a finger down at the switch stays off the stream until lifted" \
     src/app/stream/input/session_gamepad.c "bridge_switch_hold_skips_finger(gamepad, event->finger"

# --- the touchpad follows Virtual Mouse (rhoquinn8217, 2026-10-20) ---
# While Virtual Mouse lends the touchpad, two fingers pressed are the bridge
# gesture, not upstream's right click (input/bridge_override.h). Losing either
# line compiles, and every bridge presses the host's right button.
in_order "two fingers pressed are the bridge gesture while Virtual Mouse lends the touchpad" \
     src/app/stream/input/session_gamepad.c "void stream_input_handle_cbutton" \
     "bridge_override_two_finger_press_is_gesture(input)" "if (two_finger_press || touchpad_lower_right_press(state))"
in_order "... and that press's release sends nothing either" \
     src/app/stream/input/session_gamepad.c "void stream_input_handle_cbutton" \
     "if (state->physical_mouse_button < 0)" "mouse_button = state->physical_mouse_button > 0"
in_order "Virtual Mouse takes a bridged pad's touchpad as the mouse, as it takes its sticks" \
     src/app/stream/input/session_gamepad.c "void stream_input_handle_ctouchpad" \
     "!bridge_override_vmouse_lends_touchpad(input)" "if (input->touchpad_mode == TOUCHPAD_MODE_NATIVE) {"
in_order "with the TV cursor on, the touchpad moves it and tells the host the point" \
     src/app/stream/input/session_gamepad.c "void stream_input_handle_ctouchpad" \
     "if (touchpad_pointer_stream_move(input, finger_dx, finger_dy * touchpad_aspect(gamepad)))" "LiSendMouseMoveEvent(dx, dy);"
in_order "what a pad holds is noted before it is announced again" \
     src/app/stream/input/session_input.c "void stream_input_restore_gamepad" \
     "bridge_switch_hold_begin(gamepad);" "stream_input_send_gamepad_arrive(input, gamepad)"

# --- the wheel in the overlay (rhoquinn8217, 2026-10-11) --------------------
# ⓘ Upstream's file: the encoder fallback moved the focus against the wheel.
# Taking upstream's line back compiles, and the wheel runs backwards again in
# the streaming overlay.
need "the overlay's focus moves the way the wheel turns" \
     src/app/lvgl/input/lv_sdl_drv_wheel_input.c "data->enc_diff = (e.wheel.y > 0) ? -1 : 1;"

# --- the TV's cursor in the stream (rhoquinn8217, 2026-10-11) ---------------
# ⓘ Upstream's file: the stream hid the TV's cursor below webOS 9. Taking
# upstream's function back compiles, and the C1 hides it again.
need "the TV's cursor stays beside the host's in an absolute-mouse stream" \
     src/app/input/app_input.c "if (grab && app_configuration->absmouse && !app_configuration->hardware_mouse) {"

# --- the TV cursor's shortcuts and align bar (rhoquinn8217, 2026-10-11) -----
# ⓘ Lines of ours in upstream's files. Taking upstream's back compiles, and:
# root.c's, the bar that aligns the TV cursor no longer takes the keys, so the
# arrows go to the PC and the bar never sees one; session_keyboard.c's,
# Ctrl+Alt+Shift+M, Ctrl+Alt+Shift+A and Ctrl+Alt+Shift+C do nothing again on
# a keyboard webOS hands to the app (one the TV holds is
# input/bridge_keyboard.c's, a bridged one the bridge core's).
need "the bar that aligns the TV cursor takes the input as the overlay does" \
     src/app/ui/root.c "|| cursor_align_shown();"
need "Ctrl+Alt+Shift+M switches the TV cursor on a keyboard webOS hands over" \
     src/app/stream/input/session_keyboard.c "tv_cursor_toggle_mode();"
need "Ctrl+Alt+Shift+A is one of the stream's shortcuts" \
     src/app/stream/input/session_keyboard.c "{KeyComboAlignCursor,        SDLK_a, SDL_SCANCODE_A, true},"
in_order "Ctrl+Alt+Shift+A opens the align bar" \
     src/app/stream/input/session_keyboard.c "void performPendingSpecialKeyCombo" \
     "case KeyComboAlignCursor:" "cursor_align_request();"
in_order "Ctrl+Alt+Shift+C, upstream's show-the-mouse, switches the sharp and regular TV cursors" \
     src/app/stream/input/session_keyboard.c "void performPendingSpecialKeyCombo" \
     "case KeyComboToggleCursorHide:" "tv_cursor_toggle_look();"

# --- Stream actions: Vibepollo's control tiles in a menu (rhoquinn8217, 2026-10-17)
# ⓘ Lines of ours in upstream's launcher. Taking upstream's back compiles, and:
# apps.controller.c's, Resume, Terminate, Remote Input, Remote Monitor and a
# second copy of the running app are tiles in the grid again; launcher.view.c's,
# the "Stream actions" button is gone from the top bar (src/app/ui/launcher/
# stream_actions.c).
in_order "the host's controls leave each list before the grid shows it" \
     src/app/ui/launcher/apps.controller.c "static void appload_loaded" \
     "stream_actions_take(&fragment->uuid, apps);" "lv_gridview_set_data_advanced("
in_order "the running app's badge shows on every TV, not only the one that started it" \
     src/app/ui/launcher/apps.controller.c "static void appitem_bind" \
     "stream_actions_running_id(&controller->uuid, pcmanager_server_current_app(" "lv_obj_clear_flag(holder->play_indicator, LV_OBJ_FLAG_HIDDEN);"
in_order "... and on this TV's Remote Monitor or Remote Input tile (2026-10-20)" \
     src/app/ui/launcher/apps.controller.c "static void appitem_bind" \
     "if (stream_actions_tile_badged(&controller->uuid, current_id, app->base.id, holder->play_indicator)) {" "lv_obj_clear_flag(holder->play_indicator, LV_OBJ_FLAG_HIDDEN);"
in_order "a tile pressed while another app runs asks first" \
     src/app/ui/launcher/apps.controller.c "static void item_click_cb" \
     "if (stream_actions_tile_pressed(&controller->uuid, controller->apploader_apps, holder->app_id)) { return; }" \
     "int current_app = pcmanager_server_current_app(pcmanager, &controller->uuid);"
in_order "the Stream actions button sits right after the host button" \
     src/app/ui/launcher/launcher.view.c "lv_obj_t *launcher_win_create" \
     "lv_label_set_text_static(srv_chevron, MAT_SYMBOL_ARROW_DROP_DOWN);" "stream_actions_button(controller, topbar);"

# --- the server window gives the controls back (rhoquinn8217, 2026-10-17) ---
# ⓘ A fix of ours inside upstream's server.popup.c. Taking upstream's back
# compiles, and: closing "Select server" with Esc, Back, Circle or Close leaves
# the pad, the keyboard and the remote dead until a mouse click.
in_order "closing the server window without a pick hands focus back to the top bar" \
     src/app/ui/launcher/server.popup.c "static void server_popup_deleted" \
     "app_input_remove_modal_group(" "launcher_restore_nav_focus(popup->launcher);"
# ⛔ Without the second half, a window opened from it (the PIN window, a host's
# menu) crashes the app when it closes: the input's own group is left freed.
in_order "... and so does closing it under a window it opened (2026-10-22)" \
     src/app/ui/launcher/server.popup.c "static void server_popup_deleted" \
     "|| input->app_group == popup->group) {" "launcher_restore_nav_focus(popup->launcher);"

# --- the running badge and the server cards (rhoquinn8217, 2026-10-20) ---
# ⓘ Lines of ours in upstream's launcher. Taking upstream's back compiles, and:
# appitem.view.c's, the running app's badge is the large purple circle in the
# middle of the tile again; apps.controller.c's and server.popup.c's, "Select
# server" shows plain icons again, not each computer's Desktop picture
# (src/app/ui/launcher/server_cards.c).
in_order "the running badge is a ringed circle in the tile's top-left corner" \
     src/app/ui/launcher/appitem.view.c "lv_obj_t *appitem_view" \
     "lv_obj_center(play_indicator);" "stream_actions_badge_style(play_indicator, controller->col_width);"
in_order "each host's Desktop app is noted as its list loads" \
     src/app/ui/launcher/apps.controller.c "static void appload_loaded" \
     "stream_actions_take(&fragment->uuid, apps);" "server_cards_learn(&fragment->uuid, apps);"
in_order "a server tile shows its computer's Desktop picture" \
     src/app/ui/launcher/server.popup.c "static void server_tile_bind" \
     "lv_label_set_text_static(holder->icon, server_entry_icon(entry));" "server_cards_bind(item, popup->launcher->global, &entry->id,"
in_order "the pictures are let go when the server window closes" \
     src/app/ui/launcher/server.popup.c "static void server_popup_deleted" \
     "server_cards_closed();" "SDL_free(popup);"

# --- the touchpad as the TV's mouse (rhoquinn8217, 2026-10-20) ---
# ⓘ Lines of ours in upstream's root.c. Taking upstream's back compiles, and:
# no pad's touchpad moves a pointer in Aurora's screens or the overlay, or, the
# second one lost, the pointer outlives the display it was made for
# (src/app/input/touchpad_pointer.c).
in_order "the touchpad's pointer comes with the interface's display" \
     src/app/ui/root.c "void app_ui_open" \
     "app_ui_input_init(&ui->input, ui);" "touchpad_pointer_ui_opened(ui->app);"
in_order "the touchpad's pointer goes before the interface's display" \
     src/app/ui/root.c "void app_ui_close" \
     "touchpad_pointer_ui_closing();" "app_ui_input_deinit(&ui->input);"

# --- the stats' pin by keys (rhoquinn8217, 2026-10-20) ---
# ⓘ A line of ours in upstream's overlay key handler. Taking upstream's back
# compiles, and: only a pointer reaches the pin that keeps the stats on screen
# (src/app/ui/streaming/bridge_overlay.c).
in_order "Up from the overlay's row selects the stats' pin" \
     src/app/ui/streaming/streaming.controller.c "static void overlay_key_cb" \
     "lv_group_t *group = controller->group;" "if (stats_pin_overlay_key(controller, lv_event_get_key(e))) { return; }"

# --- clicking outside a window closes it (rhoquinn8217, 2026-10-20) ---
# ⓘ Lines of ours in three of upstream's windows. Taking upstream's back
# compiles, and that window then stays open when the screen around it is
# clicked (src/app/ui/common/click_away.c).
in_order "clicking outside Select server closes it" \
     src/app/ui/launcher/server.popup.c "void server_popup_open" \
     "lv_msgbox_create_i18n(NULL, locstr(\"Select server\")" "click_away_closes(msgbox);"
in_order "clicking outside the add-a-computer window closes it" \
     src/app/ui/launcher/launcher.controller.c "static void open_manual_add" \
     "lv_fragment_create_obj(fragment, NULL);" "click_away_closes(msgbox);"
in_order "clicking outside Help closes it" \
     src/app/ui/help/help.dialog.c "lv_obj_t *help_dialog_create" \
     "lv_msgbox_create(NULL, locstr(\"Help\")" "click_away_closes(msgbox);"

# --- the Settings windows, taller (rhoquinn8217, 2026-10-22) ---
# ⓘ A line of ours in each of two functions of upstream's. Taking upstream's
# back compiles, and that window is upstream's again: centred and shorter
# (src/app/ui/settings/settings_card.c). Each must come AFTER upstream's
# centring, which would otherwise move it back.
in_order "the gear's Settings window reaches from under the top bar" \
     src/app/ui/settings/settings.controller.c "lv_obj_t *settings_launcher_embedded_create" \
     "lv_obj_center(panel);" "settings_window_under_topbar(panel);"
in_order "a Settings section opens in a card from under the top bar" \
     src/app/ui/settings/settings.controller.c "static void settings_show_pane_popup" \
     "lv_obj_center(mbox);" "settings_card_under_topbar(mbox);"

# --- a keyboard types into the add-a-computer box (rhoquinn8217, 2026-10-22) ---
# ⓘ A line of ours in each of two upstream files. Taking upstream's back
# compiles, and a keyboard's keys reach the box only while it has focus, with
# webOS's on-screen keyboard over the screen (src/app/ui/common/type_into_box.c).
in_order "the add-a-computer box takes a keyboard's keys" \
     src/app/ui/launcher/add.dialog.c "static lv_obj_t *create_dialog" \
     "controller->input = ip_input;" "type_into_box(dialog, ip_input, btns, 1);"
in_order "... which the interface's key reader hands it first" \
     src/app/lvgl/input/lv_drv_sdl_key.c "static bool read_keyboard" \
     "bool pressed = event->type == SDL_KEYDOWN;" "if (type_into_box_key(event)) {"

# --- Cancel while pairing and while connecting (rhoquinn8217, 2026-10-22) ---
# ⓘ Lines of ours in upstream's pairing window, pairing worker, streaming screen,
# session worker and libgamestream. Taking upstream's back compiles, and:
# pair.dialog.c's, the PIN window has no button again and nothing closes it
# (or its late answer touches a window already freed); pairing.c's or
# libgamestream's, a Cancel leaves the request waiting for a PIN for ever;
# streaming.controller.c's, "Connecting..." has no Cancel; session_worker.c's,
# a Cancel waits for the launch, or shows "Failed to start streaming"
# (src/app/backend/pcmanager/pair_cancel.c, src/app/stream/connect_cancel.c).
in_order "the PIN window shows Cancel while it waits, and its answer comes by token" \
     src/app/ui/launcher/pair.dialog.c "static lv_obj_t *pair_dialog(lv_fragment_t *self, lv_obj_t *parent) {" \
     "static const char *btn_texts[] = {translatable(\"Cancel\"), \"\"};" "pair_result_cb, pair_cancel_begin(controller))) {"
absent_in_function "... and does not hide its button while it waits" \
     src/app/ui/launcher/pair.dialog.c "static lv_obj_t *pair_dialog(lv_fragment_t *self, lv_obj_t *parent) {" \
     "lv_obj_add_flag(controller->btns, LV_OBJ_FLAG_HIDDEN);"
in_order "a cancelled pairing's late answer finds nothing" \
     src/app/ui/launcher/pair.dialog.c "static void pair_result_cb(int result, const char *error, const uuidstr_t *uuid, void *userdata) {" \
     "pair_dialog_controller_t *controller = pair_cancel_answer(userdata);" "if (controller == NULL) {"
need "a pairing still waiting is cancelled with its window" \
     src/app/ui/launcher/pair.dialog.c "pair_cancel_end(self);"
in_order "the pairing worker's request can be cut short" \
     src/app/backend/pcmanager/worker/pairing.c "int worker_pairing(worker_context_t *context) {" \
     "pair_cancel_watch(client, context->userdata);" "pair_cancel_unwatch(client);"
need "libgamestream's requests can be cut short" \
     core/libgamestream/src/http.c "CURLOPT_XFERINFOFUNCTION, xferinfo_fn"
need "... from the app (gs_abort)" \
     core/libgamestream/src/client.c "void gs_abort(GS_CLIENT hnd) {"
in_order "Connecting... has a Cancel" \
     src/app/ui/streaming/streaming.controller.c "static bool on_event(lv_fragment_t *self, int code, void *userdata) {" \
     "lv_obj_set_height(controller->progress, LV_DPX(100));" "connecting_cancel_button(controller->progress, controller->global);"
in_order "the session's launch request can be cut short" \
     src/app/stream/session_worker.c "int session_worker(session_t *session) {" \
     "GS_CLIENT client = app_gs_client_new(app);" "connect_cancel_watch(client);"
in_order "... and a cancelled start ends with no error shown" \
     src/app/stream/session_worker.c "int session_worker(session_t *session) {" \
     "connect_cancel_done(session, client);" "gs_destroy(client);"

echo
if [ "$fail" -eq 0 ]; then
    echo "⭐ merge guard: $pass checks, nothing lost"
    exit 0
fi
echo "⛔ merge guard: $fail LOST, $pass intact"
echo "   ⚠️ A merge dropped our code. Do NOT build and test around it --"
echo "      find what took the other side and put it back."
exit 1
