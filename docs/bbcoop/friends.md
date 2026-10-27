# bbcoop: playing together (friends' guide)

Private fork for a small group (GPL-2.0+; whoever gets a build gets the source). You need your own decrypted
Bloodborne v1.09 dump (CUSA03173 EU or CUSA00900 US). Never share game files or saves.

One of you hosts: their launcher runs the server while it is open. The others join over Tailscale. Nobody
needs a server, an account on a website or an in-game password.

## 1. Tailscale (once)
1. Install [Tailscale](https://tailscale.com/download) and sign in.
2. Accept Asad's invite to the group's tailnet (an email or a link). Everyone who hosts or joins must be
   in the same tailnet.
3. Keep Tailscale connected while you play.

## 2. Get the game running (once)
- **Mac (Apple Silicon):** download `bbcoop-mac.zip`, open it, and drag Bloodborne to Applications.
  The first time, open it, click Done on the warning, then go to System Settings > Privacy & Security
  and click **Open Anyway** (the app is not signed by Apple). If macOS asks to install Rosetta, install
  it: the game is an Intel program.
- **Linux / Steam Deck:** download `Bloodborne-bbport-x86_64.AppImage`, make it executable
  (`chmod +x`) and run it.
- **Windows:** download `bbcoop-windows.zip`, extract it to `C:\` and run `Bloodborne.exe` in it
  (SmartScreen: More info, Run anyway). When Windows Firewall asks, allow Private and Public networks.

On the first start the launcher asks for your Bloodborne folder (the one with `eboot.bin` in it) and your
hunter name, the name your friends see (3 to 16 letters, digits, - or _). That is your account: a password
is made for you and kept with the launcher's settings.

## 3. Play
Launcher, **Online co-op**:
- **Host a game:** your server starts. The page shows your address; friends see you in their list as
  "<your name> — hosting". Keep the launcher open while anyone plays.
- **Join a game:** pick the host from the list (or type their address, 100.x.y.z), then Play.

In the game the host rings the Beckoning Bell and the guest the Small Resonant Bell, as in the game
(both are in your inventory); the guest arrives in the host's world. Any level and any area can join.

Everyone needs the same download: the launcher refuses to join a host on another build and says which
builds you both have.

**Chalice Dungeons:** every dungeon has its own layout, so a guest can't be pulled in from outside.
1. The host makes the dungeon at the altar while online (an older dungeon, made before your server
   kept them, has no glyph here: make a new one). Its glyph is in the altar's dungeon list.
2. A guest who is still in the host's world sees "The host went into a Chalice Dungeon": leave with a
   Silencing Blank, or quit to the title and Continue.
3. The guest, in their own Hunter's Dream: altar, **Glyph**, type the host's glyph, enter.
4. Both ring your bells in there; you join on the same layer. After that the guest follows
   the host down the layers.
A fixed chalice (Pthumeru, Hintertomb, Loran, Isz) is the same dungeon for everyone on your server, so
a guest with that chalice can also do the ritual instead of typing the glyph.

## If it doesn't connect
- Your list is empty: Tailscale must be connected on both computers, and the host's launcher must be
  open with Host picked.
- "Nobody is hosting at ...": the host's launcher is closed or not on Host.
- "The hunter name ... is taken": someone else already uses that name on that host. Pick another one
  (Online co-op, Hunter name).
- Send the Log page (launcher) to Asad.

## Hosting without the launcher (Asad)
The launcher runs the same two programs a server PC can run by hand, both reachable over Tailscale:
1. shadNet: `scripts/build-server.sh` (Linux) or `cmake --build server/build-mac`, then the binary with
   `shadnet.cfg` next to it (or in `SHADNET_HOME`): `Host=<tailscale ip>`, `BloodborneSeamlessCoop=true`,
   `RegistrationSecretKey=` (open: only the tailnet reaches it).
2. `python3 scripts/bbsc_gameserver/bbsc_gameserver.py --shadnet <ip> --host <ip> --advertise <ip>
   --bbcoop-name <name> --bbcoop-build $(git rev-parse --short=12 HEAD)` (TCP 20443 `ss.info` and the
   launchers' hello, UDP 3478 relay; level window 999 by default).
Ports: TCP 31313, 31315, 20443; UDP 31314, 3478.
