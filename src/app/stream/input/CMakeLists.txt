target_sources(moonlight-lib PRIVATE
        bridge_switch_hold.c
        keyboard_input.c
        session_input.c
        session_keyboard.c
        session_gamepad.c
        session_mouse.c
        session_pointer_gesture.c
        session_touch.c
        session_virt_mouse.c)
if (FEATURE_INPUT_EVMOUSE)
    target_sources(moonlight-lib PRIVATE session_evmouse.c)
endif ()