/*
 * The TV remote's pointer, bridged to the host as a mouse: the fork's part of
 * session_handle_input_event() in session_events.c, GuiDev1994's file, which
 * calls this once before its own switch.
 */
#pragma once

#include <stdbool.h>
#include <SDL.h>

#include "stream/session.h"

/* True when the remote's pointer is bridged and this event belongs to it: a
 * mouse movement, a button, the wheel, or one of the remote's arrow and OK
 * keys. The event has then gone to the host through the bridge, and the
 * stream's own input must not send it as well. False leaves the event to the
 * stream, as if this were not here. */
bool bridge_pointer_event(session_t *session, const SDL_Event *event);
