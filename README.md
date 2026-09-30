# Aurora - Remote DualSense Bridge Support for webOS

![platform](https://img.shields.io/badge/platform-LG%20webOS-A50034?logo=lg&logoColor=white)
![language](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)
![transport](https://img.shields.io/badge/transport-USB%2FIP-2ea44f)
![fork of GuiDev1994/aurora-tv](https://img.shields.io/badge/fork%20of-GuiDev1994%2Faurora--tv-lightgrey)

This fork of aurora-tv carries a modified version of [ciprianmisaila's
ctm-bridge-webos](https://github.com/CTM-Bridge/ctm-bridge-webos) controller
bridge. It offers a DualSense connected to a webOS TV a native connection to a
host PC with all of its features working: gyro, touchpad, **audio-based rumble**
and **speaker audio**. It also brings a range of DualSense specific features,
including **microphone support on USB**, **auto bridging** and
**[DS5Dongle](https://github.com/awalol/DS5Dongle)** support.

[GuiDev1994/aurora-tv](https://github.com/GuiDev1994/aurora-tv)'s original
functions and features work the same way. Refer to the original for support on
those.

The modified version of ctm-bridge-webos requires
[DS5-USBIP](https://github.com/rhoquinn8217/CTM-USBIP) running on your host PC
to bridge controllers.

---

## Start up guide

**Prerequisites**

| | |
|---|---|
| **DualSense** | Your controller |
| **Windows machine** | usbip-win2 and DS5-USBIP run only on Windows |
| **[vadimgrn/usbip-win2](https://github.com/vadimgrn/usbip-win2/releases)** | Install vadimgrn's usbip-win2 fork on your Windows machine, requires restart. Tested with 0.9.7.7 |
| **Streaming host** | [Sunshine](https://github.com/LizardByte/Sunshine), [Apollo](https://github.com/ClassicOldSong/Apollo), [Vibepollo](https://github.com/Nonary/Vibepollo), [Vibeshine](https://github.com/Nonary/vibeshine), etc.<br>Any Moonlight-compatible host that works with Moonlight or aurora-tv |
| **[rhoquinn8217/aurora-tv](https://github.com/rhoquinn8217/aurora-tv)** | Install the ipk on your webOS TV |

**Set up DS5-USBIP**

1. Download DS5-USBIP from the [releases page](https://github.com/rhoquinn8217/CTM-USBIP/releases).
2. Put the folder somewhere you have write access.
3. Run DS5-USBIP from that folder:

   ```powershell
   ctm-usbip.exe agent 48054 --ui
   ```

**Bridge and play**

1. Connect a DualSense to the television: via Bluetooth or USB port.
2. Start rhoquinn8217/aurora-tv on the television.
3. Turn on **Enable Device Bridging** in **Settings → USB Bridge**.
4. Start the stream to the host.
5. Press and hold the touchpad with two fingers for a second.

**DualSense is ready to use with its full feature set (microphone over USB only).**

*Note: DS5-USBIP can be set up, stopped and started through the same stream.*

---

## Why this exists

A DualSense connected to a webOS TV over Bluetooth already reached a PC with its
speaker, haptics and adaptive triggers through ciprianmisaila's bridge. A
DualSense connected to a USB port on the webOS TV did not bring its speaker,
haptics or microphone.

The difference between Bluetooth and the USB port is where the audio lives. Over
Bluetooth the controller's audio travels inside the same report stream as its
buttons and sticks, so forwarding the reports carries everything at once. Over
the USB port the controller is a composite device whose speaker, haptics and
microphone arrive as a sound card owned by webOS rather than by the
rhoquinn8217/aurora-tv app reading the DualSense. Audio through the USB port needed a different
mechanism.

---

## What this fork adds

| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; | |
|---|---|
| **DualSense audio over the TV's USB port** | A wired DualSense presents a sound card to webOS alongside its input device. This fork works out which card belongs to which controller, opens it, and carries the speaker and haptic audio out and the microphone in beside the input reports. More than one wired controller can be bridged at a time, each holding its own card. |
| **DualSense Edge support** | An Edge's reports are shaped exactly like a DualSense's, so it shares the same handling rather than duplicating it. What it needed was its own identity: it is claimed as an Edge rather than folded into the DualSense, and reported as one, which is what lets DS5-USBIP rebuild it with the Edge's own descriptor. |
| **DS5Dongle support** | A [DS5Dongle](https://github.com/awalol/DS5Dongle) presents over USB as the controller it holds, so it uses the USB path like a wired pad. Identity comes from the controller's own address rather than the adapter's serial number, so per controller settings follow the pad rather than the dongle it was plugged into. |
| **Easier bridging** | The original bridging panel is replaced by the USB Bridge panel, which presents a simple list of connected devices with quick bridge and release controls. Per DualSense settings for audio mode, headset and speaker volume, latency and haptics gain have moved to the DS5-USBIP config. Before streaming, enable bridging in the **Settings → USB Bridge** menu to use the USB Bridge panel in the streaming overlay. |
| **Auto bridge control** | New setting allows you to select specific devices or all devices to auto bridge on stream start. It is at **Settings → USB Bridge → Auto Bridge**, and the selection is kept between sessions. |
| **DualSense bridge gesture** | DualSense only: a two finger press and hold on the touchpad. One second for quick bridging, four seconds for releasing, without opening the USB Bridge panel. |
| **DualSense confirmation signals** | DualSense only: bridging, releasing and refusal events are accompanied by controller lightbar, haptic and speaker confirmation signals. Each of the signal types can be turned off in the **Settings → USB Bridge** menu. |

---

## USB Bridge Settings

Settings located at **Settings → USB Bridge**.

| &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; | |
|---|---|
| **Enable Device Bridging** | The master switch, off by default. All USB Bridge settings are disabled until it is turned on |
| **Auto Bridge** | A list of the currently connected devices, where specific devices or all devices can be marked to bridge on stream start |
| **Enable Gesture Bridging** | Enable DualSense bridging gestures. A two finger hold on the touchpad: one second bridges, four seconds releases |
| **Enable Wired Microphone** | Enables the microphone on a DualSense connected to a USB port. The controller draws on its battery while the microphone is on, whether or not anything is listening |
| **Enable BT Microphone (Unavailable)** | Disabled. A bug in webOS makes a Bluetooth DualSense's microphone unusable |
| **Disable Lightbar Bridge/Release/Refusal Signals** | Turns off the lightbar confirmation signals |
| **Disable Rumble Bridge/Release/Refusal Signals** | Turns off the rumble confirmation signals |
| **Disable Audio Tone Bridge/Release/Refusal Signals** | Turns off the audio confirmation signals |

---

## Build

The webOS package is built in Docker, so the toolchain does not have to be
installed on the host:

```sh
scripts/build-ipk.sh
```

[ctm-bridge-webos](https://github.com/rhoquinn8217/ctm-bridge-webos) must be
checked out as a **sibling directory**. This app compiles its `ctmbridge` library
straight from those sources rather than linking a prebuilt one, and the script
mounts that sibling into the container for exactly that reason. The build stops
with a message naming the expected path if it is not there.

The bridge core carries its own test suite, run by `tests/run-tests.sh` in that
repo.

---

## Acknowledgements

- **[GuiDev1994](https://github.com/GuiDev1994/aurora-tv)**: aurora-tv, the
  application this is built on. The streaming client, its interface and its webOS
  work are all GuiDev1994's.
- **[ciprianmisaila](https://github.com/ciprianmisaila)**:
  [ctm-bridge-webos](https://github.com/CTM-Bridge/ctm-bridge-webos) and
  [CTM-USBIP](https://github.com/CTM-Bridge/CTM-USBIP). The controller bridge,
  the map-driven translation pipeline, the USB/IP hosting and the DualSense audio
  work over Bluetooth are all ciprianmisaila's.
- **[mariotaku](https://github.com/mariotaku/moonlight-tv)**: moonlight-tv, the
  base both of the above are built on.

---

## License

[GNU General Public License v3.0](LICENSE) (GPL-3.0-or-later).

Copyright (C) 2026 GuiDev1994 and contributors. Fork additions copyright (C) 2026
rhoquinn8217, under the same license.

Not a license term, an ask from ciprianmisaila's CTM Bridge that this fork honours: if
you integrate CTM Bridge into your own app or fork, overlay the CTM Bridge badge
on your app's icon, the way the
[aurora-tv](https://github.com/CTM-Bridge/aurora-tv) and
[moonlight-tv](https://github.com/CTM-Bridge/moonlight-tv) forks do.

<a
href="https://github.com/CTM-Bridge/ctm-bridge-webos/blob/main/icon_extra_large.png"><img
src="https://raw.githubusercontent.com/CTM-Bridge/ctm-bridge-webos/main/icon_extra_large.png"
width="96" alt="CTM Bridge badge"></a>
