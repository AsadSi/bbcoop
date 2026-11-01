# bbcoop: getting started

bbcoop is a fan-made mod of [bbport](https://github.com/deadinside28/bloodborne_pc), a native port of
Bloodborne. It adds co-op with friends over Tailscale, a stat reset at the Doll, a weapon upgrade refund at the
workbench, and a gem slot on more firearms. It is experimental. It is not affiliated with Sony Interactive
Entertainment or FromSoftware.

## What you need

- **Your own copy of the game.** bbcoop includes no game files. It needs your own decrypted dump of Bloodborne
  with update 1.09 (CUSA03173 or CUSA00900). This guide doesn't explain how to make one, and whether you may make
  or use one depends on your country, so check that yourself.
- **Never share game files or saves:** not with friends, and not in issues or logs you post here. Everyone
  brings their own.
- **The same build as your friends.** Join refuses a host on another build.
- **[Tailscale](https://tailscale.com/download)**, to connect to each other. The launcher's server is reachable
  only by people you add. There is no public matchmaking, and bbcoop doesn't connect to PSN.

## Install

- **Mac (Apple Silicon):** download `bbcoop-mac.zip` from the
  [latest release](https://github.com/AsadSi/bbcoop/releases/latest), open it and drag Bloodborne to Applications.
  The app isn't signed by Apple. On the first start, click Done on the warning, then click **Open Anyway** in
  System Settings > Privacy & Security. Install Rosetta if macOS asks.
- **Windows:** download `bbcoop-windows.zip`, extract it to `C:\` and run `Bloodborne.exe` (SmartScreen: More
  info, Run anyway). Allow it through Windows Firewall when asked.
- **Linux:** there is no download yet, so build from source. `README.md` lists what the build needs.

```bash
git clone --recursive https://github.com/AsadSi/bbcoop.git
```

```bash
cd bbcoop && bash build.sh && bash launcher/bb-launcher.sh
```

The launcher asks for your game folder (the one with `eboot.bin` in it) and a hunter name, which is the name your
friends see (3 to 16 letters, digits, - or _). The respec and gem mod is then made on your computer from your own
game files.

## Play together

1. Everyone installs Tailscale and joins the same tailnet. One of you creates it and invites the others. Keep
   Tailscale connected while you play.
2. The host opens **Online co-op** in the launcher, picks **Host a game**, starts the game and keeps the launcher
   open.
3. Everyone else picks **Join a game**, chooses the host from the list (or types their 100.x.y.z address) and
   presses Play.
4. In the game, the host rings the Beckoning Bell and the guest rings the Small Resonant Bell. Using a ringing
   bell again stops it.

## What has been tested

| | |
|---|---|
| **Played** | A guest joining a host's world (Mac guest, Linux host), bells rung by hand, joining from another area |
| **Tested alone, not in a co-op session** | Stat reset for 1 Insight, weapon upgrade refund, gem slots on firearms, changed Fire and Bolt Paper |
| **Written, not confirmed in a co-op session** | Lamp travel together, boss fights together, shared loot, Chalice Dungeons (steps in `friends.md`), Watch camera |
| **Not tested** | Windows playing with Mac or Linux, the Linux AppImage, setting this up on a new tailnet by following this guide |

Known problems: one extra Insight can appear after a guest session that ended without a boss kill, and the lamp
or Doll can stay locked after a session.

## If something goes wrong

- Empty host list: Tailscale must be connected on both computers, and the host's launcher must be open with Host
  picked.
- "The hunter name ... is taken": pick another name on the **Online co-op** page.
- Bugs in co-op or the respec mods go to [this repo's issues](https://github.com/AsadSi/bbcoop/issues), not to
  bbport, shadPS4 or their Discord servers. Check logs for private details first, and attach no game files or saves.

## Credits and licence

GPL-2.0-or-later; some bundled parts have their own licences (`packaging/windows/licenses/`). bbcoop stands on
[bbport](https://github.com/deadinside28/bloodborne_pc), [BBSC](https://github.com/CLSAMG22/BBSC), shadp2p and
shadnet-p2p; the README's credits list the rest.
