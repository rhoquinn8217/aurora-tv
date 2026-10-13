/* The TV's volume from a controller while the streaming overlay is up.
 *
 * ⭐⭐ WHY (rhoquinn8217, 2026-10-12): "when the overlay is up i want to rebind
 * the following / right stick up -> volume up / right stick down -> volume
 * down / right stick button -> toggle mute/unmute". Asked which volume, which
 * controllers and how a held stick repeats: "1. TVs volume 2. any connected
 * controller bridged or released 3. ok".
 *
 * ➡️ So, with the overlay up and a stream running:
 *   - the right stick past half way up steps the TV's volume up, down steps
 *     it down, and held there it steps again every 0.2 seconds;
 *   - the right stick's click mutes the TV, or unmutes it.
 * The overlay reads neither (it moves with the left stick and the d-pad), so
 * nothing it does is taken away.
 *
 * ⓘ FROM WHICH CONTROLLER. One the TV holds, through SDL. A bridged one never
 * reaches SDL, so the bridge core reads its right stick and click from its own
 * report while the overlay holds input (the core's held_stick.inl, through
 * ctm_bridge_held_sticks): a DualSense, an Edge, a DS4 or an Xbox pad. A
 * bridged generic gamepad is not read, since its report does not say which
 * button is the stick's click.
 *
 * ⓘ A STICK ALREADY PUSHED WHEN THE OVERLAY OPENS DOES NOTHING until it has
 * been back at rest, so the camera stick of a game cannot change the volume
 * by itself, and neither can a stale reading.
 *
 * ⓘ THE TV'S VOLUME is its audio service: luna://com.webos.audio/volumeUp,
 * volumeDown and setMuted, checked on the C1 (webOS 6) and the B4 (webOS 9)
 * on 2026-10-12, with com.webos.service.audio's master/volumeUp and
 * master/volumeDown behind them. The calls run on a thread of their own so
 * the overlay never waits on them. What happened goes to
 * logs/overlay-volume.log. */

#pragma once

#include <stdbool.h>

struct app_input_t;

/* Start the thread the TV's calls run on. When: once the app is up. */
void overlay_volume_init(void);

/* Stop it. When: the app is closing. */
void overlay_volume_deinit(void);

/* Read the right sticks and act. `overlay_up`: the streaming overlay is shown
 * with a stream running; false forgets every stick, so the next opening starts
 * again from rest. When: every pass of the event loop. */
void overlay_volume_tick(struct app_input_t *input, bool overlay_up);
