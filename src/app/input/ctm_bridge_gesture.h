/* Plug a controller by a gesture on the controller itself -- the mirror of the
 * unplug gesture, which lives in the bridge core. See the .c file for why the
 * two directions are detected differently. */

#ifndef CTM_BRIDGE_GESTURE_H
#define CTM_BRIDGE_GESTURE_H

#include <SDL.h>
#include <stdbool.h>   /* the file declares bool-returning functions */

struct app_input_t;

#ifdef __cplusplus
extern "C" {
#endif

#if defined(TARGET_WEBOS)

/* Look at every open controller's current state. When: once per pass of the
 * app's event loop.
 *
 * Deliberately a tick rather than an event handler. Fingers resting still on
 * the touchpad generate no motion events, so an event-driven check cannot time
 * a hold -- it would take one reading and never hear anything again. The loop
 * also runs regardless of where controller events are routed, which differs
 * between streaming and not.
 *
 * Cheap: reads state SDL has already collected, no allocation, no blocking. */
struct session_t;

/* `session` may be NULL when no stream is running -- the gesture still works,
 * and the moonlight side simply has nothing to be told. */
void ctm_bridge_gesture_tick(struct app_input_t *input, struct session_t *session);

/* Forget a controller's gesture progress. When: it is removed, or bridged --
 * once bridged, the bridge core's own gesture takes over. */
void ctm_bridge_gesture_reset(SDL_JoystickID id);

/* ⭐⭐ ASK FOR A BRIDGE AS IF THE CHORD HAD BEEN HELD.
 *
 * The panel used to plug a device itself, and the results diverged from the
 * gesture in ways that were not obvious until they bit: the emulated pad was
 * never retired, so the host saw the controller twice; the watcher did not know
 * it owned the bridge, so it never restored anything afterwards; and unbridging
 * from the panel skipped the sequence that ends a bridge properly.
 *
 * Rather than copy that sequence into the panel and let the two drift, the
 * panel asks for the SAME code to run. This sets exactly what a completed chord
 * sets and returns; the pulse, the plug, the confirmation, the retiring of the
 * emulated pad and the ownership flag all follow on the next ticks, unchanged.
 *
 * Returns false when no controller is behind that node -- a keyboard or a mouse
 * is not an SDL controller and has no gesture path to borrow. */
bool ctm_bridge_gesture_request_bridge(const char *node);

/* ⭐⭐ SHIPS AS 0: the host's lightbar writes are DROPPED while this side is
 * drawing a pattern.
 *
 * ⓘ Moonlight's emulated pad has a lightbar, so Windows and Steam paint it and
 * the colour arrives over the stream. Set to 1 to forward them unconditionally,
 * which is how it behaved before 2026-08-19 -- useful only for telling a
 * host-driven colour apart from one of ours. */
#define CTM_HOST_OWNS_LIGHTBAR 0

/* True while this side is drawing a pattern on that controller's lightbar. */
bool ctm_bridge_gesture_light_busy(SDL_GameController *controller);

/* The player number SDL gave the controller behind this hidraw node, or -1 if
 * nothing there is a controller. Zero-based, as SDL reports it.
 *
 * ⭐ The panel gets its list from the bridge core, which knows nothing about
 * SDL. The hidraw node is what both sides speak, so it is the join. */
int ctm_bridge_gesture_player_for_node(const char *node);

#else

#define ctm_bridge_gesture_tick(input, session) ((void)0)
#define ctm_bridge_gesture_reset(id) ((void)0)

#endif

#ifdef __cplusplus
}
#endif


/* ⭐⭐ SILENCE EVERY CONTROLLER'S MICROPHONE, BEFORE SDL OPENS ANY OF THEM.
 *
 * Declared here rather than in a bridge-core header because this is the only
 * one of ours the app already includes -- adding an include to upstream's
 * app.c for one call would be a wider change than the call itself.
 *
 * ⚠️ MUST BE CALLED BEFORE SDL's controller subsystem starts. A DualSense told
 * to stream microphone audio keeps doing it when a program dies -- it forgets
 * only when its Bluetooth link drops. An app that crashed while one was
 * streaming comes back to find SDL reading encoded sound as sticks and
 * buttons. Measured 2026-08-13; the menus activated themselves until the
 * controller was powered off.
 *
 * Nothing in this app arms a microphone. This exists for the state we cannot
 * cause and could not otherwise escape. */
void ctm_mic_safety_disarm_all(void);

#endif /* CTM_BRIDGE_GESTURE_H */
