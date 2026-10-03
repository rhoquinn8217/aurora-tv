/*
 * The fork's calls from app.c, GuiDev1994's application file, gathered here so
 * that app.c carries one line for each instead of the code and its reasoning.
 * Each is called from exactly one place in app.c, named beside it below.
 */
#pragma once

#include <stdbool.h>

#include "app.h"

/* In app_init(), BEFORE SDL_Init(): a DualSense that a crash left streaming
 * microphone audio is told to stop before SDL can read it as input. */
void bridge_app_before_sdl(void);

/* In app_init(), with the other webOS SDL hints: PlayStation controllers are
 * asked for their full input report, which the bridge's gesture reads. */
void bridge_app_sdl_hints(void);

/* In app_init(), once the interface and the bus are up: the command port. */
void bridge_app_started(app_t *app);

/* First thing in app_deinit(): the command port goes. */
void bridge_app_stopping(void);

/* In app_process_events(), after SDL's events are filtered: the bridge's
 * gesture and the TV's keyboard grab take their turn. */
void bridge_app_events(app_t *app);
