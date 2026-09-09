/* The Auto Bridge window: which devices bridge themselves when a stream starts.
 *
 * ⭐ A CUT-DOWN USB Bridge panel, opened from the USB Bridge settings pane
 * (rhoquinn8217, 2026-09-08). Device, address, a box each, and one button.
 *
 * ⛔ NO BRIDGE OR RELEASE HERE, and that is not a limitation being worked
 * around. Bridging is HOST-BOUND: session.c hands the bridge the streaming
 * host's address and only then starts it, so outside a stream there is no host
 * to bridge to and the action would mean nothing. A MARK carries no host, which
 * is exactly why it is the one thing that can be set beforehand.
 *
 * ➡️ So this shares the panel's shape but none of its state vocabulary: no
 * BASIC/FULL, no arrows, no Bridge all. A window offering an action that cannot
 * work is worse than one that does not offer it. */

#ifndef AUTO_BRIDGE_WINDOW_H
#define AUTO_BRIDGE_WINDOW_H

#if defined(TARGET_WEBOS)

/* Open it. Builds from the devices connected right now and takes the d-pad
 * until it closes, then hands focus back where it found it. */
void auto_bridge_window_open(void);

#endif

#endif /* AUTO_BRIDGE_WINDOW_H */
