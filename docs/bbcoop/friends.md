# bbcoop: playing together (friends' guide)

Private fork for a small group (GPL-2.0+; whoever gets a build gets the source). You need your own decrypted
Bloodborne v1.09 dump (CUSA03173 EU or CUSA00900 US). Never share game files or saves.

## 1. Get connected (once)
1. Install [Tailscale](https://tailscale.com/download) and sign in. Asad invites you to the group's tailnet
   (accept the invite). Check: `tailscale ping 100.106.120.120` answers.
2. Ask Asad for your NP ID and password (an account on the group's own server) and the in-game multiplayer
   password. They are not PSN or Seamless Hunt accounts. The protocol is not encrypted, so use a throwaway password.

## 2. Install and run
- Linux / Steam Deck: `distrobox enter bbcoop`, then `./build.sh` and `python3 launcher/bbport_launcher.py --play`.
- Mac (Apple Silicon): `scripts/mac-deps.sh`, `./build.sh`, then `python3 launcher/bbport_launcher.py --play`
  (details in `CLAUDE.md`, "macOS").
- Launcher, Online page: Play online on, **Seamless co-op (BBSC)** on, your NP ID and password. The server fields
  are already filled in (`100.106.120.120:31313`, WebAPI `:31315`, game server `http://100.106.120.120`).
- In game: Options > Network > Password: the shared multiplayer password.

## 3. Co-op
The host rings the Beckoning Bell (Hunt Bell), the guest rings the Small Resonant Bell (Kin Bell), both
with the same password. Any level can join: the server's co-op level window is 999.

## If it doesn't connect
- `tailscale status` must show the server PC (`steamdeck`) online; the server must be running (Asad's PC).
- `BB_GPU_LOG=info` logs these lines when it works: `P2P relay fallback: our public endpoint`,
  `'<name>' reached us through the relay`, `BBSC hunters: 2 in the world - 1 cooperator(s)`.
- No echo from the server on UDP 31314 means the server is too old: gen 20 clients frame their signaling
  with a 6-byte header (`FF 83 FF FF FF FF`) that only a server built since commit "answer gen 20's 6-byte
  signaling frame" understands.

## Hosting the server (Asad)
Two programs on one machine, both reachable over Tailscale:
1. shadNet: `scripts/build-server.sh`, then `server/build-linux/shadnet` with `shadnet.cfg`: `Host=0.0.0.0`,
   `BloodborneSeamlessCoop=true`, `RegistrationSecretKey=<private>`; accounts with `shadnet-sample ... register`.
2. `python3 scripts/bbsc_gameserver/bbsc_gameserver.py --shadnet <tailscale ip> --host 0.0.0.0 --advertise <tailscale ip>`
   (TCP 20443 `ss.info`, UDP 3478 relay; level window 999 by default).
Ports: TCP 31313, 31315, 20443; UDP 31314, 3478. Allow them on the tailscale interface only.
shadNet alone has no 3478 relay and no `ss.info`; the game server provides both.
