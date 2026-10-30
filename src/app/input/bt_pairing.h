/* The TV's Bluetooth controllers, from Aurora: list them, forget them, search
 * and pair a fresh one, without the TV's Settings.
 *
 * ⭐⭐ WHY (rhoquinn8217, 2026-10-29): "when a controller is paired to the TV, if
 * that controller is pair to another device, the TV has a hard time pairing
 * back with it. Much faster to just forget and repair from scratch every time
 * and if we can do it without using webOS settings menu that is very slo, that
 * is what I want." The panel they asked for (ui/common/bt_panel.h) lists the
 * TV's devices, forgets one or all of them, and has a search switch that turns
 * itself off once a device has paired.
 *
 * ⓘ THE SERVICE is the one the TV's own "Connect Bluetooth Controller" page
 * calls, read out of the Settings app on webOS 8.3 (2026-10-29):
 * luna://com.webos.service.bthidmanager/
 *   getPairedDeviceLists                  the registered devices
 *   requestDelete {"deviceAddr": ...}     forget one
 *   requestDiscovery, cancelDiscovery     the search
 *   subscribeDiscovery                    what the search finds: a status
 *                                         (startDiscovery, endDiscovery) or a
 *                                         device, by "address" and "name"
 *   requestConnect {"name","address"}     pair one it found
 *   subscribeDevicesStatus                "paired", "connected" or "fail"
 * The same methods are on the C1 (webOS 6.5.3) and the B4 (9.2.4), in a group
 * webOS marks private, and on the B4 they answer the app (build 587).
 *
 * ⓘ Every call is made on a thread of this file's, so the screens never wait
 * on the TV, and every call and answer goes to logs/bt-pairing.log. */

#pragma once

#include <stdbool.h>

#define BT_PAIRING_MAX 8

typedef struct {
    char name[96];
    char addr[24];
    bool gamepad;
    bool connected;
} bt_pairing_device_t;

/* What the panel draws, copied out in one piece. */
typedef struct {
    bt_pairing_device_t devices[BT_PAIRING_MAX];
    int count;
    bool listed;        /* the TV has answered with a list at least once */
    bool searching;     /* the search switch */
    char status[200];   /* one line: what is happening, or what went wrong */
} bt_pairing_state_t;

/* One call to the service, waited for up to five seconds. Returns the TV's
 * answer, for the caller to free, or NULL when the method is not one of the
 * service's above, the call could not be made, or nothing came back in time.
 * When: from the control port; it blocks, but never for longer than that. */
char *bt_pairing_call(const char *method, const char *payload);

/* Whom to tell when anything changes: called on the UI thread, with nothing
 * held. One listener, the panel; NULL when it closes. */
void bt_pairing_set_listener(void (*changed)(void));

/* The state as it stands. When: on the UI thread, from the listener. */
void bt_pairing_state(bt_pairing_state_t *out);

/* Ask the TV for its list again. */
void bt_pairing_refresh(void);

/* Forget one device by its address, or every device in the list with NULL. */
void bt_pairing_forget(const char *addr);

/* The search switch. On, the TV searches until a device it does not already
 * have pairs, which turns the switch off again; it searches again each time a
 * round ends without one, for up to three minutes. */
void bt_pairing_search(bool on);
