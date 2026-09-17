#include "bridge_request.h"

#if defined(TARGET_WEBOS)

#include "ctm_bridge_gesture.h"

bridge_request_result_t bridge_request_device(const ctm_bridge_dev_t *dev)
{
    if (dev == NULL) {
        return BRIDGE_REQUEST_FAILED;
    }
    /* ⭐⭐ ASK THE GESTURE FIRST, and plug directly only when it cannot help.
     *
     * Plugging directly diverged from the chord in ways that were invisible
     * until they bit: the emulated pad was never retired, so the host saw the
     * controller twice; the watcher did not know it owned the bridge, so it
     * never restored anything afterwards; and releasing then skipped the
     * sequence that ends a bridge properly, which over Bluetooth looked like
     * the controller powering itself off.
     *
     * ⚠️ The direct plug is the fallback for a device with no SDL controller
     * behind its node: keyboards and mice, whose input the core takes away
     * from the TV by grabbing it. ⓘ Until 2026-09-13 the gesture found only a
     * controller SDL opened through hidraw, so a wired Xbox pad or a pad SDL
     * read through evdev landed here too and kept its emulated pad. It now
     * joins those through their input device -- see controller_path_is_node.
     *
     * ⓘ No check that the node is set: an empty node is refused by the gesture
     * and still reaches the direct plug, which is how the TV remote's row has
     * always been bridged. */
    if (ctm_bridge_gesture_request_bridge(dev->node)) {
        return BRIDGE_REQUEST_ASKED;
    }
    return ctm_bridge_plug_index(dev->index) ? BRIDGE_REQUEST_PLUGGED : BRIDGE_REQUEST_FAILED;
}

void bridge_release_device(const ctm_bridge_dev_t *dev)
{
    if (dev == NULL) {
        return;
    }
    /* ⛔ One device, never ctm_bridge_unplug_all(): that is the app SHUTDOWN
     * path, and it tears every session down at once while holding the device
     * lock. */
    ctm_bridge_unplug_index(dev->index);
}

const char *bridge_request_result_name(bridge_request_result_t result)
{
    switch (result) {
        case BRIDGE_REQUEST_ASKED:
            return "asked";
        case BRIDGE_REQUEST_PLUGGED:
            return "plugged";
        case BRIDGE_REQUEST_FAILED:
        default:
            return "failed";
    }
}

#endif /* TARGET_WEBOS */
