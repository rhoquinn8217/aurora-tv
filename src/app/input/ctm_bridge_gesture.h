/* Plug a controller by a gesture on the controller itself -- the mirror of the
 * unplug gesture, which lives in the bridge core. See the .c file for why the
 * two directions are detected differently. */

#ifndef CTM_BRIDGE_GESTURE_H
#define CTM_BRIDGE_GESTURE_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(TARGET_WEBOS)

/* Look at one controller's current state. When: whenever moonlight already has
 * a controller event in hand, so idle controllers cost nothing. Cheap: reads
 * state SDL has already collected, no allocation, no blocking. */
void ctm_bridge_gesture_poll(SDL_GameController *controller, SDL_JoystickID id);

/* Forget a controller's gesture progress. When: it is removed, or bridged --
 * once bridged, the bridge core's own gesture takes over. */
void ctm_bridge_gesture_reset(SDL_JoystickID id);

#else

#define ctm_bridge_gesture_poll(controller, id) ((void)0)
#define ctm_bridge_gesture_reset(id) ((void)0)

#endif

#ifdef __cplusplus
}
#endif

#endif /* CTM_BRIDGE_GESTURE_H */
