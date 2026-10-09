# Aurora - with Remote DualSense Bridge Support for webOS

![platform](https://img.shields.io/badge/platform-LG%20webOS-A50034?logo=lg&logoColor=white)
![language](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)
![transport](https://img.shields.io/badge/transport-USB%2FIP-2ea44f)
![fork of GuiDev1994/aurora-tv](https://img.shields.io/badge/fork%20of-GuiDev1994%2Faurora--tv-lightgrey)

This fork of aurora-tv carries a modified version of [ciprianmisaila's
ctm-bridge-webos](https://github.com/CTM-Bridge/ctm-bridge-webos) controller
bridge. It offers a DualSense connected to a webOS TV a connection to a host PC
over the network or the internet, with native features: gyro, touchpad,
**audio-based rumble** and **speaker audio**.
It also brings a range of DualSense specific features, including
**microphone support on USB**, **auto bridging** and
**[DS5Dongle](https://github.com/awalol/DS5Dongle)** support.

Some webOS TVs do not support DualSense over Bluetooth. Use a USB port on
those, or a [DS5Dongle](https://github.com/awalol/DS5Dongle) to stay wireless.

The modified version of ctm-bridge-webos expects
[DS5-USBIP](https://github.com/rhoquinn8217/DS5-USBIP) running on your host PC
to bridge controllers.

## Start up guide

**Prerequisites**

| Requirement | Notes |
|:---|:---|
| **DualSense** | Your controller |
| **Windows** | Your host machine's operating system |
| **Moonlight-compatible host** | Streaming software on that Windows machine:<br>[Sunshine](https://github.com/LizardByte/Sunshine), [Apollo](https://github.com/ClassicOldSong/Apollo), [Vibepollo](https://github.com/Nonary/Vibepollo), [Vibeshine](https://github.com/Nonary/vibeshine), etc. |
| **[rhoquinn8217/aurora-tv](https://github.com/rhoquinn8217/aurora-tv)** | Install the ipk (pending) on your webOS TV |

**Set up DS5-USBIP**

1. Download the installer from the [releases page](https://github.com/rhoquinn8217/DS5-USBIP/releases) (pending).
2. Run the installer. It is unsigned, so you need to select **Run anyway**.<br>
   *Note: installation includes the required
   [usbip-win2](https://github.com/vadimgrn/usbip-win2/releases) driver and will
   require a restart.*
3. Start DS5-USBIP from the Start menu. It lives in the tray.

**Bridge and play**

1. Connect a DualSense to the television: via Bluetooth or USB port.
2. Start rhoquinn8217/aurora-tv on the television.
3. Turn on **Enable Device Bridging** in **Settings (⚙️) → USB Bridge**.
4. Start the stream to the host.
5. Press and hold the touchpad with two fingers for a second.
6. DS5-USBIP will open showing that the DualSense is natively connected.<br>
   *Optional (Recommended): Create and set a new "DS5-DS4-touchpad-to-mouse"
   pre-set and try it out.*

**Start using the DualSense with gyro, touchpad, audio-based rumble and
speaker audio (microphone on USB only).**

*Note: DS5-USBIP can be set up, stopped and started through the same stream.*

## USB Bridge Settings

Located at **Settings (⚙️) → USB Bridge**.

| Setting | &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; What it does |
|:---|:---|
| **Enable Device Bridging** | The master switch, off by default. All USB Bridge settings are disabled until it is turned on. Turn it on before streaming to use the USB Bridge panel in the streaming overlay |
| **Auto Bridge** | A list of the currently connected devices, where specific devices or all devices can be marked to bridge on stream start. The selection is kept between sessions |
| **Enable Gesture Bridging** | Enable DualSense bridging gestures. A two finger hold on the touchpad: one second bridges, four seconds releases |
| **Enable Wired Microphone** | Enables the microphone on a DualSense connected to a USB port. The controller draws on its battery while the microphone is on, whether or not anything is listening |
| **Enable BT Microphone (Unavailable)** | Disabled. A bug in webOS makes a Bluetooth DualSense's microphone unusable |
| **Disable Lightbar Bridge/Release/Refusal Signals** | Turns off the lightbar on a bridge, a release and a refusal |
| **Disable Rumble Bridge/Release/Refusal Signals** | Turns off the rumble on a bridge, a release and a refusal |
| **Disable Audio Tone Bridge/Release/Refusal Signals** | Turns off the tone on a bridge, a release and a refusal |

## Why this exists

A DualSense connected to a webOS TV over Bluetooth already reached a PC with its
speaker, rumble and adaptive triggers through ciprianmisaila's bridge. A
DualSense connected to a USB port on the webOS TV did not bring its audio
features.

The difference between Bluetooth and the USB port is where the audio lives. Over
Bluetooth the controller's audio travels inside the same report stream as its
buttons and sticks, so forwarding the reports carries everything at once. Over
the USB port the controller is a composite device whose audio arrives as a sound
card owned by webOS rather than by the rhoquinn8217/aurora-tv app reading the
DualSense.

This fork's goal is to cover this gap for DualSense controllers and at the same
time make bridging easy and robust.

## What this fork adds

| Addition | &nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; What it does |
|:---|:---|
| **DualSense audio over the TV's USB port** | A wired DualSense presents a sound card to webOS alongside its input device. This fork works out which card belongs to which controller, opens it, and carries the speaker and rumble audio out and the microphone in beside the input reports. More than one wired controller can be bridged at a time, each holding its own card. |
| **DualSense Edge support** | An Edge's reports are shaped exactly like a DualSense's, so it shares the same handling rather than duplicating it. What it needed was its own identity: it is claimed as an Edge rather than folded into the DualSense, and reported as one, which is what lets DS5-USBIP rebuild it with the Edge's own descriptor. |
| **DS5Dongle support** | A [DS5Dongle](https://github.com/awalol/DS5Dongle) presents over USB as the controller you've paired it to, so it uses the USB path like a wired pad. Identity comes from the controller's MAC address rather than the adapter's serial number, so when set to auto bridge, the app sees the same controller whether it's connected via Bluetooth, USB or with the DS5Dongle. |
| **Easier bridging** | The original bridging panel is replaced by the USB Bridge panel, which presents a simple list of connected devices with quick bridge and release controls. Per DualSense settings for audio mode, headset and speaker volume, latency and haptics gain have moved to the DS5-USBIP config, and a DS5-USBIP button beside the panel opens the **Controller Configs** window on the host for whichever device is bridged. |
| **Auto bridge control** | New setting allows you to select specific devices or all devices to auto bridge on stream start. |
| **DualSense bridge gesture** | DualSense only: a two finger press and hold on the touchpad. One second for quick bridging, four seconds for releasing, without opening the USB Bridge panel. |
| **DualSense confirmation signals** | DualSense only: bridging, releasing and refusal events are accompanied by controller lightbar, rumble and speaker confirmation signals. |

## Build

The webOS package is built in Docker, so the toolchain does not have to be
installed on the host:

```sh
scripts/build-ipk.sh
```

[ctm-bridge-webos](https://github.com/rhoquinn8217/ctm-bridge-webos) is a
**submodule** of this repository, at `third_party/ctm-bridge-webos`, beside the
six that GuiDev1994's aurora-tv already carries. This app compiles its
`ctmbridge` library straight from those sources rather than linking a prebuilt
one, and each commit of this repository records the exact commit of the core it
is built with. A clone made with `--recursive` has it; in any other checkout,
`git submodule update --init --recursive` fetches it. If the folder is empty,
the build stops and names that command.

The bridge core carries its own test suite, run by `tests/run-tests.sh` in that
repo.

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

## License

[GNU General Public License v3.0](LICENSE) (GPL-3.0-or-later).

Copyright (C) 2026 GuiDev1994 and contributors. Fork additions copyright (C) 2026
rhoquinn8217, under the same license.

Not a license term, an ask from ciprianmisaila's CTM Bridge that this fork honours: if
you integrate CTM Bridge into your own app or fork, overlay the CTM Bridge badge
on your app's icon, the way the
[aurora-tv](https://github.com/CTM-Bridge/aurora-tv) and
[moonlight-tv](https://github.com/CTM-Bridge/moonlight-tv) forks do.
