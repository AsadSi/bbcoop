Bloodborne co-op (bbcoop) for Windows
=====================================

No game files are included. You need your own decrypted dump of Bloodborne version 1.09,
CUSA03173 (EU) or CUSA00900 (US): the folder with eboot.bin, sce_module, sce_sys, dvdroot_ps4.

Requirements
- Windows 10 (1903 or later) or Windows 11, 64-bit.
- An Intel Core of the 4th generation (Haswell) or newer, or an AMD Ryzen.
- A Vulkan 1.3 graphics card with a current driver.
- Tailscale (tailscale.com/download), signed in to the group's network, to play together.

Starting
1. Unzip the whole folder somewhere short, e.g. C:\bbcoop (not inside Program Files).
2. Double-click Bloodborne.exe. If Windows says "Windows protected your PC": More info, Run anyway.
3. The first time it asks for your Bloodborne folder (the one with eboot.bin) and your hunter name.
4. Press PLAY. The first start prepares the game and builds the respec mod: give it a few minutes.
- Play Bloodborne.exe starts the game straight away with the launcher's saved settings.
- In the game, Insert (or L3+R3 on a gamepad) opens the port's menu.

Playing together (launcher, Online co-op)
- Host a game: this PC runs the server while the launcher is open. Friends see you in their list.
- Join a game: pick the host from the list (or type their 100.x.y.z address), then PLAY.
- The host rings the Beckoning Bell, the guest the Small Resonant Bell; the guest arrives in the host's world.
- When Windows Firewall asks about Bloodborne, python or shadnet: tick both Private and Public
  networks and click Allow. Without it nobody can join you.
- Everyone needs the same download. The launcher says so when two builds differ.

When something goes wrong, send these files (from the folder you unzipped):
- user\last_run.log                  the last game start
- user\log\bbsc-timeline-*.txt        the newest one: what happened in co-op
- host\logs\*                         when you were hosting
- out\respec_mod.log                  when the respec mod was not built

Data
- Saves: user\ next to Bloodborne.exe. Settings: bbport.ini next to it; launcher options in
  %APPDATA%\bbcoop-launcher. Keep user\ when you unzip a newer version over this one.

Source: bbcoop is GPL-2.0-or-later; whoever gets this build gets the source too. Built on bbport by
deadinside28, its Windows port and launcher by Supermedo (github.com/Supermedo/bloodborne_pc), BBSC by
Classmg22, and python.org's Python (licenses\Python-LICENSE.txt).
