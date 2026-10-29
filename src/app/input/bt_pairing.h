/* The TV's Bluetooth controllers, from Aurora: list them, forget them, search
 * and pair a fresh one, without the TV's Settings.
 *
 * ⭐⭐ WHY (rhoquinn8217, 2026-10-29): "when a controller is paired to the TV, if
 * that controller is pair to another device, the TV has a hard time pairing
 * back with it. Much faster to just forget and repair from scratch every time
 * and if we can do it without using webOS settings menu that is very slo, that
 * is what I want." The panel they asked for lists the TV's controllers, forgets
 * one or all of them, and has a search switch that turns itself off once a
 * controller has paired.
 *
 * ⓘ THE SERVICE is the one the TV's own "Connect Bluetooth Controller" page
 * calls, read out of the Settings app on webOS 8.3 (2026-10-29):
 * luna://com.webos.service.bthidmanager/
 *   getPairedDeviceLists                  the registered controllers
 *   requestDelete {"deviceAddr": ...}     forget one
 *   requestDiscovery, cancelDiscovery     the search
 *   subscribeDiscovery                    what the search finds
 *   requestConnect {"name","address"}     pair one it found
 *   subscribeDevicesStatus                paired, connected or failed
 * The same methods are on the C1 (webOS 6.5.3) and the B4 (9.2.4), in a group
 * webOS marks private.
 *
 * ⚠️ FIRST STEP ONLY. This makes one call and hands back the TV's answer, for
 * the control port's `bt` command: whether webOS lets Aurora use the service
 * at all decides everything else, and only the app itself can find out. The
 * panel follows once rhoquinn8217 has seen its picture. */

#pragma once

/* One call to the TV's Bluetooth controller service, waited for up to five
 * seconds. Returns the TV's answer, for the caller to free, or NULL when the
 * method is not one of the service's above, the call could not be made, or
 * nothing came back in time. Every call and answer goes to logs/bt-pairing.log.
 * When: from the control port; it blocks, but never for longer than that. */
char *bt_pairing_call(const char *method, const char *payload);
