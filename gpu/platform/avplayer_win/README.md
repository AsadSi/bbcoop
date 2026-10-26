# Movie player on Windows (bbcoop)

Supermedo/bloodborne_pc's AvPlayer (shadPS4's, with Supermedo's fixes; `gpu/shadps4/core/libraries/avplayer`
and `videodec/video_utils.*` on its `windows` branch, 5adea35), GPL-2.0-or-later, copied unchanged but for
the SDK version query it did not use. Built only on Windows (`gpu/CMakeLists.txt`); Linux and macOS keep
upstream 0.6's player in `platform/avplayer`.

Why: on Windows the game's movie audio thread (`Mv_AudioPlayThread`) found its object destroyed with
upstream 0.6's player ("Mutex is not initialized", docs/bbcoop/windows.md), and Supermedo's Windows builds
play the same movies with this one.
