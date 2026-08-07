/* Plug a controller by a gesture on the controller itself -- the mirror of the
 * unplug gesture, which lives in the bridge core. See the .c file for why the
 * two directions are detected differently. */

#ifndef CTM_BRIDGE_GESTURE_H
#define CTM_BRIDGE_GESTURE_H

#include <SDL.h>

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
void ctm_bridge_gesture_tick(struct app_input_t *input);

/* Forget a controller's gesture progress. When: it is removed, or bridged --
 * once bridged, the bridge core's own gesture takes over. */
void ctm_bridge_gesture_reset(SDL_JoystickID id);

#else

#define ctm_bridge_gesture_tick(input) ((void)0)
#define ctm_bridge_gesture_reset(id) ((void)0)

#endif

#ifdef __cplusplus
}
#endif

#endif /* CTM_BRIDGE_GESTURE_H */
