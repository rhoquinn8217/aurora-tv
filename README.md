# Wired DualSense/DualSense Edge Bridge Support

![platform](https://img.shields.io/badge/platform-LG%20webOS-A50034?logo=lg&logoColor=white)
![language](https://img.shields.io/badge/C-11-00599C?logo=c&logoColor=white)
![transport](https://img.shields.io/badge/transport-USB%2FIP-2ea44f)
![upstream](https://img.shields.io/badge/fork%20of-GuiDev1994%2Faurora--tv-lightgrey)

> **A fork of [aurora-tv](https://github.com/GuiDev1994/aurora-tv) by GuiDev1994,
> carrying the [ctm-bridge-webos](https://github.com/CTM-Bridge/ctm-bridge-webos)
> controller bridge by Ciprian Teodor Misaila.**
>
> **Everything about what aurora-tv is, how to build it, how to install it and
> how to use it lives upstream** —
> [read it there](https://github.com/GuiDev1994/aurora-tv#readme). It is his
> project and his documentation, and it is kept current in a way a copy here
> would not be.
>
> This file covers only what this fork adds.

---

## What this fork adds

Upstream bridges a DualSense over Bluetooth. This fork carries that work
forward in two directions: a cable, and everything that is not a DualSense.

### A controller on a cable arrives whole

Plugged into the television, a DualSense or DualSense Edge reaches the gaming
PC as a native USB device, with the parts a passthrough leaves behind:

| | |
|---|---|
| **Speaker and haptics** | Carried over the wire, so a game's audio and its haptic track play on the pad itself |
| **Adaptive triggers** | The game's own trigger effects, not an approximation |
| **Microphone** | Read from the controller's own capture device and offered to the host as its own Windows microphone |

### Any controller, and more than controllers

Bridging is no longer a DualSense feature. A DualShock 4, an Xbox pad, a
third party pad in either mode, a keyboard and a mouse all bridge through the
same path, most of them with no code written for them specifically. A device
with several interfaces, such as a wireless keyboard and mouse sharing one
receiver, is handled as one device rather than as unrelated parts.

### Choosing what crosses, and when

| | |
|---|---|
| **A device list on the television** | One row per device, bridged or released individually, with bridge all and release all |
| **Auto bridge** | Mark a device and it crosses by itself when the stream starts |
| **A gesture on the pad** | Hold the touchpad chord to hand a controller over, and again to take it back, without reaching for a menu |
| **Battery** | A pad that reports its charge shows it, read from the pad's own reports rather than guessed from a bucket |

### The controller tells you what happened

A bridge is confirmed in your hand rather than only on screen: a tone and a
pulse when a pad crosses, a second when it comes back, and a distinct pattern
when a bridge is refused. A pad with a lightbar says the same thing in colour.
Pads with no speaker of their own are signalled by the television instead, so
every controller answers and not only the DualSense.

### On the Windows side

The host half is [DS5-USBIP](https://github.com/CTM-Bridge/CTM-USBIP), which
hosts the USB/IP device and adds per controller configuration: button
remapping, gyro, touchpad and stick to mouse, an on screen keyboard driven
from the pad, and adaptive trigger effects. It has its own page.

---

## Acknowledgements

- **GuiDev1994** — [aurora-tv](https://github.com/GuiDev1994/aurora-tv), the
  application this is built on. The streaming client, its interface and its
  webOS work are all his.
- **Ciprian Teodor Misaila** —
  [ctm-bridge-webos](https://github.com/CTM-Bridge/ctm-bridge-webos) and
  [CTM-USBIP](https://github.com/CTM-Bridge/CTM-USBIP). The controller bridge,
  the map-driven translation pipeline, the USB/IP hosting and the DualSense
  audio work over Bluetooth are all his — this fork extends one transport, it
  did not build the thing.
- **[mariotaku](https://github.com/mariotaku/moonlight-tv)** — moonlight-tv, the
  base both of the above are built on.

---

## License

[GNU General Public License v3.0](LICENSE) (GPL-3.0-or-later).

Copyright (C) 2026 GuiDev1994 and contributors.
Fork additions copyright (C) 2026 rhoquinn8217, under the same license.

Not a license term, an upstream ask this fork honours: if you integrate CTM
Bridge into your own app or fork, overlay the CTM Bridge badge on your app's
icon, the way the
[aurora-tv](https://github.com/CTM-Bridge/aurora-tv) and
[moonlight-tv](https://github.com/CTM-Bridge/moonlight-tv) forks do.

<a href="https://github.com/CTM-Bridge/ctm-bridge-webos/blob/main/icon_extra_large.png"><img src="https://raw.githubusercontent.com/CTM-Bridge/ctm-bridge-webos/main/icon_extra_large.png" width="96" alt="CTM Bridge badge"></a>
