# Chiaki-ng PS3

![Chiaki-ng PS3](ps3/pkgfiles/ICON0.PNG)

**PS4 and PS5 Remote Play on your PlayStation 3.** Play your PS4 or PS5 games on the TV where your
PS3 is, with a DualShock 3. This is an unofficial port of [chiaki-ng](https://github.com/streetpea/chiaki-ng),
the open-source Remote Play client, to the PS3, built with the open-source
[ps3dev](https://github.com/ps3dev) toolchain.

| | |
|---|---|
| ![Console list](ps3/docs/menu-consoles.png) | ![Settings](ps3/docs/menu-settings.png) |

## Features

- PS4 and PS5, 720p at 60 fps, decoded by the PS3's hardware video decoder
- **Internet Remote Play via PSN (NAT UDP hole punching)** — play your remote PS5/PS4 outside your home network with **zero router or console configuration on the PS5 side**
- Automatic PSN OAuth2 token refresh and cloud PS5 console discovery right from the PS3
- Finds your consoles on the local network (LAN) and wakes them from rest mode
- Pair a console with its PIN from the PS3 itself (or auto-authenticate via PSN)
- Stereo audio with automatic drift correction, so the sound stays in sync
- DualShock 3 with analog L2/R2 and vibration
- Frame pacing settings: lowest delay, balanced or smoothest

## Requirements

- A PS3 with custom firmware that can install homebrew packages (tested on Rebug + Cobra)
- A PS4 or PS5 on your local network **or** remotely over the Internet (signed in to PSN with Remote Play enabled). **Wired Ethernet** for the PS3 is strongly recommended.
- Your PSN account ID (and PSN OAuth2 refresh token for Internet Remote Play; see below)

Not supported: PS5 HEVC/1080p (the PS3 decoder only does H.264 up to 720p here), microphone, touchpad gestures, motion controls.

## Install

1. Download `chiaki-ps3.pkg` from the [latest release](../../releases/latest).
2. Copy it to a USB stick, plug it into the PS3 and install it with **Package Manager → Install Package Files**.
3. Start **Chiaki-ng PS3** from the Game column.

## Set up

### Internet Remote Play (PS5/PS4 outside your home — no setup on PS5 side)

1. On your Mac or PC, run:
   ```bash
   python3 ps3/tools/psn_login.py chiaki.cfg
   ```
   Sign in to your PSN account in the browser and paste the redirect URL. The script fetches your PSN account ID, refresh token, and remote PS5 DUID and writes `chiaki.cfg`.
   *(Already using chiaki-ng on a PC/Mac? You can also export its `.ini` and run `python3 ps3/tools/export_keys.py exported.ini > chiaki.cfg`.)*
2. Put `chiaki.cfg` in the root folder of a USB stick, start **Chiaki-ng PS3**, and import it (automatic on first run, or via **START → Import chiaki.cfg from USB**).
3. Select your remote PS5 (shown as `PSN (Internet)`) and press **✕** — the PS3 wakes it via PSN, punches through NAT automatically, and starts streaming. If you are behind a strict/symmetric NAT, enable **START → NAT port guessing (PSN): On**.

### Local Network (LAN)

**Pair a console (once).** Select your console in the app and press ✕.

1. The first time, the app asks for your **PSN account ID** and opens the PS3's on-screen keyboard.
   The ID is a short code like `AbCdEfGhIjk=`; chiaki-ng on a PC or Mac shows it after you log in to PSN
   in its settings. Capital and small letters matter. You can also enter it later under
   START (Settings) → PSN account ID.
2. Then enter the 8-digit PIN from **PS4: Settings → Remote Play Connection Settings → Add Device** or
   **PS5: Settings → System → Remote Play → Link Device**.

Already using chiaki-ng on a PC or Mac? Skip the pairing: export its settings to an `.ini` file, run
`python3 ps3/tools/export_keys.py exported.ini > chiaki.cfg`, put `chiaki.cfg` in the top folder of a USB
stick and choose **Settings → Import chiaki.cfg from USB**. This copies your account ID and all consoles you
paired there. (While the app has no settings of its own yet, a `chiaki.cfg` on a USB stick is imported automatically at start.)

**Play.** Select the console and press ✕. A console in rest mode is woken up first. For that, enable
"Stay Connected to the Internet" and "Enable Turning On PS4/PS5 from Network" in the console's
rest-mode power settings.

## Controls

| DualShock 3 | PS4 / PS5 |
|---|---|
| Buttons, D-pad, sticks, L1/R1, L3/R3 | same |
| L2 / R2 | L2 / R2 (analog) |
| START | OPTIONS |
| SELECT (tap) | SHARE / Create |
| SELECT + START | PS button |
| SELECT + ✕ | touchpad click (L1 / R1 instead of ✕: left / right half) |
| SELECT + △ | show / hide the stats overlay (frame rate, delay, bitrate) |
| **SELECT + ○ (hold 1 s)** | **end the stream** |

In the menu: ✕ connect, △ console options, □ add a console by IP address, START settings, ○ quit.

## Tips

- Put your TV in **Game Mode**. TV picture processing often adds more delay than everything else.
- Settings → Bitrate: "Automatic" suits most networks; on wired, a higher value gives a sharper picture.
  On a weak network, try a lower bitrate or 540p.
- Settings → Video pacing: "Balanced" (default) shows the most frames, "Lowest delay" reacts fastest.
- If something goes wrong, `/dev_hdd0/game/CHIAKIPS3/USRDIR/chiaki.log` says why (the log of the run before is kept as `chiaki.prev.log`). Please attach them to bug reports.

## Building, technical details

See [ps3/README.md](ps3/README.md) for the build instructions and notes on how the port works.
This tree is chiaki-ng (upstream commit `43728af`) plus the PS3 port in `ps3/` and small `__PS3__` changes in
`lib/`. The original chiaki-ng README is in [README.chiaki-ng.md](README.chiaki-ng.md).

## License and credits

AGPL-3.0, the same as chiaki-ng (see `COPYING` and `LICENSES/`). Based on
[chiaki-ng](https://github.com/streetpea/chiaki-ng) and the original [Chiaki](https://git.sr.ht/~thestr4ng3r/chiaki)
by Florian Märkl and contributors. Not affiliated with or endorsed by Sony Interactive Entertainment.
"PlayStation", "PS3", "PS4", "PS5" and "DualShock" are trademarks of Sony Interactive Entertainment.
