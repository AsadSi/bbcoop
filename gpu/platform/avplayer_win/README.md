# Movie player on Windows (bbcoop)

Supermedo/bloodborne_pc's AvPlayer (shadPS4's, with Supermedo's fixes; `gpu/shadps4/core/libraries/avplayer`
and `videodec/video_utils.*` on its `windows` branch, 5adea35), GPL-2.0-or-later, copied unchanged but for
the SDK version query it did not use. Built only on Windows (`gpu/CMakeLists.txt`); Linux and macOS keep
upstream 0.6's player in `platform/avplayer`.

Why: on Windows the game's movie audio thread (`Mv_AudioPlayThread`) found its object destroyed with
upstream 0.6's player ("Mutex is not initialized", docs/bbcoop/windows.md), and Supermedo's Windows builds
play the same movies with this one.

Changed here (marked `bbcoop`), from a review of the copy (2026-10-26):
- `AvPlayerState::Start()` stopped a playback under way through `Stop()`, which takes the source lock again
  (shared) behind its own write lock: it hangs. It also started with no source (closed meanwhile) or
  before the streams were known. Now it refuses those, stops under the lock it holds, and tells the game
  "Play" after releasing it.
- The decoded frame's GPU invalidation covered `width` (aligned to 16) bytes a row; `CopyNV12Data` writes
  `pitch` (aligned to 64).
- The stored stream indexes are FFmpeg's; six places looked them up again in the list of supported streams.
- The game froze on a black screen after the intro movie (2026-10-26). Fixed: a stop reaches a decoder
  that another thread is already joining (`gpu/shim/core/libraries/kernel/threads.h`); `Stop()`,
  `Pause()` and `Resume()` tell the game after releasing the source lock, and `Stop()` holds it
  exclusively while it clears the frames; a stop after the threads ended by themselves still clears
  what they left; events are emitted outside the event lock, and none while the player closes.
