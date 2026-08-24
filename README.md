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
