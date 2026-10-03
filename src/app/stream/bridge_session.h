/*
 * The fork's part of a stream's start and stop, gathered here so that
 * session.c, GuiDev1994's file, carries one call for each instead of the code
 * and its reasoning. Each function names the place in session.c that calls it.
 */
#pragma once

#include <stdbool.h>

#include "stream/session.h"

/* In session_start_input(), on the virtual mouse's line: false while Bridge
 * Override holds the TV's own mouse controls off (input/bridge_override.h). */
bool bridge_session_vmouse_allowed(void);

/* In session_start_input(), after the input has started: the bridge's
 * settings go in, then the bridge starts, and the devices marked for Auto
 * Bridge are bridged. */
void bridge_session_started(session_t *session);

/* In session_stop_input(), after the input has stopped: the bridge stops and
 * every controller shows its player colour again. */
void bridge_session_stopped(void);

/* At the top of session_toggle_vmouse(): a press of Virtual Mouse with Bridge
 * Override on switches the override off and the virtual mouse on, and is not a
 * toggle. True when that is what it did. */
bool bridge_session_vmouse_pressed(session_t *session);

/* Is the TV's own mouse mode driving the cursor right now? So that a caller
 * does not have to reach into the session's input. */
bool bridge_session_vmouse_active(session_t *session);
