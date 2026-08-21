# Aurora — wired DualSense support

A fork of [Aurora](https://github.com/GuiDev1994/aurora-tv) by GuiDev1994,
with the [CTM Bridge](https://github.com/CTM-Bridge/CTM-USBIP) controller
bridge, adding **full wired DualSense support**.

A DualSense plugged into the TV appears on the gaming PC as a native USB
DualSense — with its speaker, haptics, adaptive triggers and microphone.

> # ⛔ NOT CURRENT — A PRE-RELEASE DRAFT. DO NOT READ THIS AS THE STATE OF THE APP.
>
> _Marked 2026-08-14._ **This file was never published and describes the fork as
> it stood months ago.** ⛔ *"Nothing has been removed or changed"* **is no longer
> true** — auto-plug is off, gestures were added, the pad is released per
> controller, and there is extensive Bluetooth work.
>
> ⓘ **The repository's own README is still upstream's** and is deliberately left
> alone for now.
>
> ➡️ **The real README gets written at RELEASE** (rhoquinn8217, 2026-08-14): our
> features, with a link back to `aurora-tv` for the base ones. → **T-108**
>
> 🔗 **For what the fork actually does:** `improvements-over-upstream.md`,
> `project-status.md`.

> ⭐ **Everything else is Aurora.** Nothing has been removed or changed.
> For streaming settings, controls, screenshots, install and the build guide,
> see the [upstream README](https://github.com/GuiDev1994/aurora-tv#readme).
> This page covers only what is added.

---

## What is added

Over a cable. The Bluetooth path is unchanged.

| | |
|---|---|
| **Speaker, haptics, adaptive triggers** | The wired path carries input only without this |
| **Microphone** | Read from the controller and presented to the host |
| **Plug and unplug by gesture** | Two fingers on the touchpad, press down. **One second to bridge, four to release** |
| **The controller says what happened** | A tone and a pulse when bridged and when released; three short rumbles when a plug is refused |
| **Each controller gets its own audio** | With two identical controllers, the right one is identified rather than guessed |
| **Audio recovers itself** | If the controller's audio device fails mid-session |

## What is unchanged

⭐⭐ **A controller you have not upgraded behaves exactly as it does in
Aurora**, including when the bridge is running. Upgrading is opt-in, per
controller, and by gesture.

⚠️ **Without the CTM-USBIP listener running on the PC, this is Aurora.**
Controllers still work through Moonlight's normal path — a plug attempt simply
fails, and the controller buzzes three times to say so.

## Requirements

- Everything Aurora requires.
- A DualSense or DualSense Edge **connected to the TV by USB cable**.
- The [CTM-USBIP](https://github.com/CTM-Bridge/CTM-USBIP) listener running on
  the gaming PC.

## Install

Build from source as per the
[upstream build guide](https://github.com/GuiDev1994/aurora-tv/blob/main/docs/BUILD_WEBOS.md),
or install the `.ipk` with
[Device Manager](https://github.com/webosbrew/dev-manager-desktop).

## Credits

- Base: [mariotaku/moonlight-tv](https://github.com/mariotaku/moonlight-tv)
- Aurora: [GuiDev1994/aurora-tv](https://github.com/GuiDev1994/aurora-tv)
- Controller bridge: [CTM-Bridge/CTM-USBIP](https://github.com/CTM-Bridge/CTM-USBIP)

## License

[GNU General Public License v3.0](LICENSE) (GPL-3.0-or-later), as with Aurora
and moonlight-tv. Copyright and attribution details are in
[COPYRIGHT](COPYRIGHT).
