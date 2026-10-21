Bloodborne co-op (bbcoop) for Windows
=====================================

No game files are included. You need your own decrypted dump of Bloodborne version 1.09,
CUSA03173 (EU) or CUSA00900 (US): the folder with eboot.bin, sce_module, sce_sys, dvdroot_ps4.

Requirements
- Windows 10 (1903 or later) or Windows 11, 64-bit.
- An Intel Core of the 4th generation (Haswell) or newer, or an AMD Ryzen (AVX, BMI1, MOVBE, LZCNT).
- A Vulkan 1.3 graphics card with a current driver.

Starting
- Bloodborne.exe opens the launcher. On "Game & effects" pick your game folder, then press PLAY.
- Play Bloodborne.exe starts the game straight away with the settings saved in the launcher.
- In the game, Insert (or L3+R3 on a gamepad) opens the port's menu.

Co-op
- "Online co-op": tick "Play online", keep "Seamless Hunt (BBSC)" and "Seamless co-op (BBSC)", and
  enter a shadNet account (register at seamlesshunt.com). "Test connection" checks the servers.
- Everyone needs game version 1.09, the same server and bbcoop (Linux, macOS or Windows).
- bbcoop's rules: enemies +50% HP and +10% damage per extra player, shared loot, a guest who dies
  comes back at the host's lamp. Respec at the Doll (1 Insight) and weapon refunds at the workbench.

Data
- Saves and shader caches: user\ next to Bloodborne.exe (the launcher can pick another folder).
- Settings: bbport.ini next to Bloodborne.exe; launcher options in %APPDATA%\bbcoop-launcher.
- The log of the last start: user\last_run.log.

Source: bbcoop is GPL-2.0-or-later; whoever gets this build gets the source too. Built on bbport
by deadinside28 and its Windows port and launcher by Supermedo (github.com/Supermedo/bloodborne_pc).
