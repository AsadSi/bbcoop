# Duplicate in Load Game: reverse-engineering notes (v1.09)

Asad asked for this on 2026-10-12: pick a character in the title screen's Load Game list and press Triangle (Duplicate). The
character is copied into the first free profile slot as "<name> (2)" (then "(3)" and so on). The copy appears in the list as
a separate character, and every copy is kept until it is deleted. To go back to a copy, load it; from then on it autosaves
into its own slot. Code: `src/runtime_saveslots.c`. Addresses are image offsets (ELF vaddr), unless marked as XML addresses.

## Save files (`user/savedata/<user>/CUSA00900/SPRJ0005/`)
- **Files:** `userdata0000..0009` are the 10 character slots (0x140000 bytes each). `userdata0010` is the system data
  (0x40000 bytes). Nothing is checksummed: the SaveLoad2 library's digest flag (descriptor+0x134 bit 0) is off.
- **What a slot file stores:** no slot number and no ID. An unused slot is all zeros.
- **Header:** u32 0x41, then the last map (u32), then play time in ms, then a section table.
  - PlayerGameData sits at a different offset in each file: 0x8454 in Hoonter's slot, 0xB320 in TEST's.
  - The name is at PlayerGameData +0xA8: UTF-16, 16 characters, a terminator.
- **`userdata0010`** (version 0x1a, parsed by 0x14eb470):
  - +0x108C: `exists[10]`.
  - +0x1096 + i·0x192: slot i's summary (name, level, play time, map, lamp, face, equipment).
  - The summaries are a cache, rebuilt from the player data whenever the character saves (0x14f9490).
- **How the files are written:** every read and write is a request on FrpgSaveLoadMan (`[0x556d438]`); no buffer is kept
  between requests.
  - A load (0x1831e90) reads `userdataN` from disk every time; `backupN` is the fallback if that fails.
  - A save renames `userdataN` to `backupN` and then writes `userdataN`.
  - So a file copied on the host is picked up by the next load of that slot, with no restart.
- **In memory:** the system data is GameDataMan (`[0x553b130]`). The profile list is at `[+0x58]`: `exists[i]` at +0x10+i
  and 0x220-byte records at +0x20+0x220·i (name first, 0x22 bytes).
  - The Load Game list is built from this memory copy (0x1c93bd0).
  - The system save (`0x156d8a0(0,0,0)`, run by the job 0x1c2cf10 → 0x1c2aa10) writes it to `userdata0010`. It writes only
    entry 17.

## The Load Game list (FrpgMenuDlgProfileSelectList, "ProfileList")
- **How it opens:** title "Load Game" → 0x1b479f0 → the list opener 0x1b39520(…, allowDelete = 1, onSelect). The dialog is
  built in 0x1b409d0.
- **Buttons:** each is a binding added with 0x1ad5730(dialog, keyguideItem, &action, &predicate). The list has two:
  - Cross loads.
  - Square deletes. That binding is only added when allowDelete is set; the block is 0x1b42ecf..0x1b42f81. Its action
    vtable is 0x533b5e0, whose call is **0x1b43230**.
  - Triangle (key guide 0x1ae1f30, `KG_R_U`) is free here.
- **The Delete action 0x1b43230:**
  1. It reads the slot (r14d) and builds the Yes/No box with text 204/406013.
  2. Then it chains these steps:
     - **job A** 0x1b45de0: sets `exists[slot] = 0` if slot ≤ 9, and clears the last-played slot if it was this one;
     - the list refresh 0x1b45440;
     - the system save job;
     - a step that closes the dialog when the list is empty.
  - The chain stops on No.
  - The slot file is not touched.

## The hooks
| Site | Bytes | What it does |
|---|---|---|
| 0x1b42f86 (after the Delete binding) | `48 8B BD 60 E9 FF FF` | Adds a Triangle binding labelled 202/400090 "Duplicate", reusing the idle predicate (rbx). Its action is a std::function target `{vtable, dialog}`. Our vtable has its own clones (they keep our vtable) and our call; slots 3..7 are the Delete action's. Our call runs 0x1b43230 with `duplicating` set. |
| 0x1ae8cd1 (menu text lookup) | respec's hook | While `duplicating`, 204/406013 becomes 406090 "Duplicate selected profile?", or 406091 when no slot is free. The hook is shared: `runtime_respec.c` calls `saveslots_text` first, and without respec's hooks Duplicate stays off. |
| 0x1b4334e (job A's slot captured) | `44 89 B5 88 FE FF FF` | While `duplicating`, the slot is tagged: +0x100, or +0x200 when no slot is free. The game's own clear in job A then does nothing (it only handles slots up to 9, and the tag never matches the last-played slot). |
| 0x1b45ded (in job A, after Yes) | `48 8B 51 58 48 85 D2` | For a +0x100 tag, it takes the first free slot and copies `userdataM` to `userdataN` on the host (temp file, then rename). The name is changed wherever it appears in the file as a whole padded field (PlayerGameData +0xA8, and in some saves a second copy beside the face data). It deletes `backupN`, copies the 0x220-byte record with the new name, and sets `exists[N]`. The game's chain then saves `userdata0010` and refreshes the list. |

- **Save directory:** the host directory comes from `runtime_savedata_last_host("SPRJ0005")` (a bbcoop addition in
  `runtime_savedata.c`).
- **Text:** `tools/respec_mod.py` adds 400090 to entry 202 (the key guide) and 406090/406091 to entry 204. Without them (no
  respec mod) the label lookup has no text (raw pointer +0 NULL) and the button is left out.
- **Turning it off:** `BB_SAVESLOTS=0`.
