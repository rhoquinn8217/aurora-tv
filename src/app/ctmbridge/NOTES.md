# Why this fork changes these files of GuiDev1994's aurora-tv

This fork adds a DualSense bridge to GuiDev1994's
[aurora-tv](https://github.com/GuiDev1994/aurora-tv). Most of it lives in files
of its own. Where it has to change one of GuiDev1994's files, it leaves one line
at the place, in this form:

    /* This fork: <what it does>. Why: ctmbridge/NOTES.md, "<section>". */

The section of that name, below, holds the reasoning, so the file itself
carries as few lines of the fork as possible. `tests/merge-guard.sh` checks
after a merge that each change is still in place, and that every line of this
kind names a section that exists here.

---

## A pad opened twice

**Where:** `src/app/input/input_event.c`, `app_input_handle_event()`, on
`SDL_JOYDEVICEADDED`.

This is the same guard upstream gives the controller event below it. It is
needed since v1.2.10, which has three other openers: the scan at init, at
session start and before launch, and `SDL_CONTROLLERDEVICEADDED`. A pad the scan
has already opened arrives here again on its own queued event. Without the
guard it is opened a second time and takes a second `gs_id`, so the launch mask
carries two bits and the host makes two pads for one controller.

Measured on 2026-09-21 on an LG C1, in Apollo's own log: `Gamepad 0 will be
DualShock 4` and `Gamepad 1 will be DualShock 4`, two milliseconds apart, with
a single DualSense Edge connected.

**Merge guard:** "a pad already opened is not opened again".

## The host's lightbar

**Where:** `src/app/input/input_gamepad.c`,
`app_input_gamepad_set_controller_led()`, which returns while
`ctm_bridge_gesture_light_busy()` says the bridge is drawing on that
controller.

Moonlight's emulated pad has a lightbar, so Windows and Steam paint it. That
colour arrives over the stream and lands on the physical controller through
this function. So the host is a second writer on that light the whole time a
stream is running, whatever the bridge does.

That is not the host taking the light. The call is the app's, and the TV
chooses to pass it on, in the same way that the overlay stays reachable while
streaming. So it can equally choose not to, and it does not while the bridge is
drawing one of its signals: two writers on one light is what every flicker
report turned out to be, and no amount of care on one side fixes a second
writer.

Once a controller is bridged, its emulated pad is retired. Nothing comes through
here for it at all, and the host owns the light properly, over its own
connection.

The writes are always dropped while a signal draws. A switch that forwarded
them anyway served the search for the flicker, and was removed on 2026-09-15.

Upstream v1.2.10 looks the pad up by `gs_id` rather than by its slot in the
array, because the host's `controllerNumber` is the `gs_id` and the slots are
sparse, the same correction it made to rumble. That is taken as it stands, and
the check here sits on the controller it hands back.

**Merge guard:** "the host's lightbar waits while a bridge signal draws".

## Upstream's wired DualSense feedback

**Where:** `src/app/input/input_gamepad.c`, the four calls into
`dualsense_usb.c`: the lightbar, the adaptive triggers, the player LEDs and the
microphone LED. Each is skipped while `ctm_bridge_gesture_pad_is_ours()` says
the bridge is using the pad.

Upstream v1.3.0 added its own wired DualSense feedback, `dualsense_usb.c`, which
writes the host's lightbar, trigger and LED packets straight to hidraw. That is
a second writer on the same node and the same report as the bridge, and its
state is sticky: every write re-asserts every field it has ever been given, so
a trigger packet repaints the lightbar too.

So it stays off a pad the bridge is using: while a signal draws, and while the
pad is bridged. The same check sits on all four entry points, because any one
of them re-asserts the lightbar. Outside those windows it runs exactly as
upstream wrote it.

**Merge guard:** "upstream's wired feedback stays off a pad the bridge is
using, at all four entry points".

## Select and Start held back

**Where:** `src/app/stream/input/session_gamepad.c`, `CHORD_GATED_BUTTONS` and
`CHORD_GATE_HELD`, used in `stream_input_handle_cbutton()`.

Select and Start, View and Menu on an Xbox pad, are the two buttons of the
overlay's chord that the host acts on by itself: Steam opens its on-screen
keyboard and an app switcher from them. Every press goes to the host the moment
it arrives, and the chord is only recognised once all four buttons are down, so
rolling through the chord left Steam's keyboard and the app switcher sitting
behind the overlay, every time.

The rule, from rhoquinn8217 on 2026-09-18: while both bumpers are held, these
two buttons belong to the chord and are not sent at all. Nobody holds LB and RB
together and then reaches for Start in a game, so nothing real is taken away,
and it costs no latency anywhere, which a delay on the bumpers would have.

It does mean the chord is pressed bumpers first. Press View before the bumpers
are down and the host still sees it, because at that moment it is an ordinary
press and there is no way to know otherwise. The button state is still updated,
so the chord check that follows a later press still sees them. Only the send is
withheld.

**Merge guard:** "Select and Start are held back while both bumpers are down".

## One controller left out

**Where:** `src/app/stream/input/session_gamepad.c`,
`stream_input_gamepad_sends_moonlight()`, and the `moonlightExcludedMask` field
in `src/app/stream/input/session_input.h`.

The bridge takes a controller away from Moonlight one controller at a time, not
for the whole session. A switch for the whole session used to do this whenever
the bridge was enabled, and it turned Moonlight's gamepad input off for every
controller, even ones nobody had bridged, leaving them unusable for no reason.
It was never set after 2026-08-19, and it has been removed.

The per-controller mask is the only suppression there is, and upstream's code
has none at all, so a stream behaves exactly as upstream's does until a
controller is actually bridged, and only that controller changes.

The mask is stored, deliberately, rather than asked of the bridge on every send.
Deriving the answer live meant calling into the bridge from inside Limelight's
send path, and that crashed the app on 2026-08-10. The working prototype stored
it too.

The announce at stream start needs no check of the fork's: upstream's
`stream_input_send_gamepad_arrive()` asks
`stream_input_gamepad_sends_moonlight()`, which reads the mask, so a controller
that is still bridged when a stream reconnects is refused there.

**Merge guard:** "bridged controllers excluded from Moonlight", and "the
announce asks whether a controller is excluded".

## Handing a controller over, in order

**Where:** `src/app/stream/input/session_input.c`,
`stream_input_exclude_gamepad()` and `stream_input_restore_gamepad()`.

The order inside each is what makes them work. Done the other way round, both do
nothing: the send paths consult the mask, so a remove sent after the bit is set
would be refused, and an arrive sent before the bit is cleared would be refused
too. Each line carries a short note saying which comes first.

`started` is checked because a controller can be bridged before a stream
begins. There is nothing to tell the host at that point: the mask is enough,
and the announce at stream start refuses a controller whose bit is set.

The remove path itself, `stream_input_send_gamepad_remove()` in
`session_gamepad.c`, deliberately does not consult the mask. Being handed to the
bridge is the commonest reason to send a remove, and refusing there would leave
the host holding a pad that never sends anything again.

**Merge guard:** "a controller is removed from the host before its bit is set",
"its bit is cleared before it is announced again", and "the remove path does
not consult the mask".

## The overlay's keyboard shortcut

**Where:** `src/app/stream/input/session_keyboard.c`,
`stream_input_handle_key()`.

The overlay's shortcut acts at once. It does not wait for every key to be up.
On 2026-09-15 (build 346) rhoquinn8217 found that typing worked until the
shortcut was used, and then the keyboard stopped working for good. A pending
combo ignores every key press until SDL's own keyboard state shows all keys
released, and on webOS a key release does not always arrive (see
`session_events.c`). So one key stayed down, the combo never ran, and the
keyboard was dead for the rest of the stream.

So the shortcut releases on the host whatever it saw pressed, opens the
overlay, and leaves nothing pending. The releases still to come land on the
overlay, or on the stream as harmless key-ups.

The shortcut is Ctrl+Alt+Shift+S, Moonlight's stats shortcut, which has always
opened the overlay here. O sat beside it from 2026-09-15 to 2026-10-01, added in
the belief that there was none, and this was written for it. A bridged
keyboard's S is found by the bridge core.

**Merge guard:** "the overlay's shortcut acts at once".

## Reconnect after a network drop

**Where:** `src/app/stream/session_worker.c`: `session_worker()`, the
`connect:` label in it, `session_worker_reconnect_allowed()` and
`session_worker_reconnect_wait()`.

A stream that dies with a network error is resumed in place. There is no
`USER_STREAM_CLOSE` or finish, so input and the bridge stay up, and the screen
shows "Connecting..." instead of going back to the launcher. It is bounded by
attempts and by time, and a stream that stayed up for a while resets the
attempt budget, so an occasional blip never exhausts it. This came with Ciprian
Misaila's original integration of the bridge.

At the `connect:` label upstream's own code runs as upstream wrote it. v1.2.10
refreshes the pads before launch, so a pad whose arrival the app never heard is
still found. v1.3.0 launches Sunshine and Apollo with `gcmap=0` and learns the
pads from Controller Arrival instead, because a non-zero mask plus Arrival made
the host allocate two ViGEm pads for one physical controller on first connect;
GFE still needs the bitmap at launch. The fork's retry label, and its `ret` and
`gamepad_mask` declared above the label, are kept. The rest is upstream's.

**Merge guard:** "a stream that drops resumes in place".

## The player colour on an app switch

**Where:** `src/app/app.c`, `app_event_filter()`, on
`SDL_APP_WILLENTERBACKGROUND`. The fork adds no code here, only the line that
says why.

The player colour is not painted on this event, and it was tried. Painting
here puts the colour up before the teardown that follows it, so a bridged
controller went blue and then black as the bridge came down (measured
2026-08-19). `session_stop_input()` paints instead, after the bridge has
actually stopped, which is the right moment both for this path and for a
normal end of a stream.

If the colour does not appear on an app switch, the teardown never reached
`session_stop_input()`. That is worth knowing, and it has been an open
question for a while.

## Stream boost

**Where:** `src/app/ui/settings/panes/experimental.pane.c`, the "Stream boost"
row, and `src/app/stream/session_worker.c`, which reads it before the
connection starts.

The stream boost restarts a rooted LG 34SR65QC at every stream start. Until it
was a setting, the only way round that was a second package built for rooted
sets, and that cost the same mistake twice: two builds were overwritten,
because only the path that made the rooted package took a new build number.
The setting retires the second package, so one build serves every set.

The row is shown on rooted sets only. On an unrooted TV the boost never runs,
so the row would be a control that does nothing.

**Merge guard:** "the stream boost is a setting".
