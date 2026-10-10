# Respec: reverse-engineering notes (v1.09)

All addresses are image offsets (XML `Address` − 0x400000). For both loadable segments of `out/eboot.elf`, file offset = image offset + 0x4000. The work was done on 2026-10-10 by disassembling the eboot and reading the game's own params. No public offsets were used.

## Player data
- **GameDataMan global** `0x553b130`. The local PlayerGameData is `*(*(u64*)(base+0x553b130) + 8)` (null-checked), as Lua's `GetLocalPlayerSoulLv` (0x1337250) and `GetHeroPoint` (0x1338670) read it.
- **The local player character** is WorldChrMan `0x553e878` → `+0x60`.

PlayerGameData fields (int32 unless noted). The internal names are Dark Souls leftovers; the field list comes from the debug dump at 0x14f3e40.

| Field | Offset |
|---|---|
| Vitality | 0x40 |
| Endurance | 0x48 |
| Strength | 0x58 |
| Skill | 0x60 |
| Bloodtinge | 0x68 |
| Arcane | 0x70 |
| Insight copy | 0x80 |
| Insight (0–99) | 0x84 |
| Hunter level (SoulLv) | 0x90 |
| Blood Echoes held (≤ 999,999,999) | 0x94 |
| Echoes earned in total | 0x98 |
| Origin index (u8, 0–8) | 0xce |

- **Level invariant:** SoulLv = the sum of the six stats − 50.

## Origins
The origin's row is `CharaInitParam[3000 + origin index]`. The start values are applied by 0x14e8fe0.

| # | Origin | Lv | Vit | End | Str | Skl | Bld | Arc |
|---|---|---|---|---|---|---|---|---|
| 0 | Milquetoast | 10 | 11 | 10 | 12 | 10 | 9 | 8 |
| 1 | Lone Survivor | 10 | 14 | 11 | 11 | 10 | 7 | 7 |
| 2 | Troubled Childhood | 10 | 9 | 14 | 9 | 13 | 6 | 9 |
| 3 | Violent Past | 10 | 12 | 11 | 15 | 9 | 6 | 7 |
| 4 | Professional | 10 | 9 | 12 | 9 | 15 | 7 | 8 |
| 5 | Military Veteran | 10 | 10 | 10 | 14 | 13 | 7 | 6 |
| 6 | Noble Scion | 10 | 7 | 8 | 9 | 13 | 14 | 9 |
| 7 | Cruel Fate | 10 | 10 | 12 | 10 | 9 | 5 | 14 |
| 8 | Waste of Skin | 4 | 10 | 9 | 10 | 9 | 7 | 9 |

## Level-up cost
- **Function:** `int GetLevelUpCost(int targetLevel)` at `0x157f050` is a leaf function. It computes in float32: x = target + 80, `cost = (int)(x²·(0.1 + max(0, x−92)·0.02) + 1.0 + 0.5)`, using CalcCorrectGraph row 200.
- **Inlined copies:** the level-up menu at 0x1b2e460 has the same code inlined.
- **Spot checks:** 11 costs 829, 12: 847, 13: 1039, 20: 2601, 100: 60265.
- **Community formula:** it matches only for levels ≥ 12.
- **Rounding:** float32 rounding differs from exact maths by 1 for some levels above 260, so call the game's function instead of porting it.
- **Refund:** Σ cost(n) for n from the origin level + 1 up to the current level.

## Applying stats
- **Commit task:** `0x1c13d00(void *f)` is the commit task the level-up menu queues. It reads int32 values at f+0x08 (level), +0x0c Vit, +0x10 End, +0x14 Str, +0x18 Skl, +0x1c Bld and +0x20 Arc (from a 0x28-byte buffer).
- **What it does:** writes them to PlayerGameData, then recalculates HP, stamina and the other derived values, and refreshes the player character.
- **What it doesn't:** it leaves Echoes and Insight alone, so write those directly.
- **Thread:** it runs on the game thread.

## The Doll's talk script (`script/talk/m21_00_00_00.talkesdbnd.dcx`)
- **Who's who:** t210304 is the Plain Doll (`c8030_0000`), and t210692 is the Workshop tools (Fortify Weapon, Repair, Blood Gem, Arcane Haze).
- **Commands, as bank_index:**
  - `c1_19(slot, text, flag)` adds a menu entry.
  - `c1_48()` opens the Fortify Weapon list (menu type 13, closed when `f59(13,0)==0`).
  - `c1_11(flag, 0/1)` sets an event flag, read back with `f15(flag)`.
  - `c1_17(2, text, 3, 4, 2)` shows a Yes/No dialog (`f22()`: 1 = yes).
- **Text:** menu text lives in menu.msgbnd entry 30, EventText.
- **New entries:** 15009100 "Reset Attributes", 15009110 "Refund Weapon Upgrades", and dialog 10019100.
- **Flags:** 72100900 means respec requested; 72100901 means refund mode. The 72100xxx block belongs to the Hunter's Dream NPCs and is saved. 72100500–999 is not referenced by any talk script, event script, param or the eboot.
- **soulstruct 2.6 quirks:**
  - Its BB talk writer cross-links state machines that reuse state ids, so `tools/respec_mod.py` has its own writer.
  - Binder entry names need `PureWindowsPath`.
  - Command names are Dark Souls 1 names, so use the raw ids.

## Weapons, upgrades and the inventory
- **Weapon IDs:** the upgrade level is in the ID, `family*10000 + level*100` (+0..+10). Uncanny = +10000 and Lost = +20000. Firearms and DLC weapons follow the same scheme.
- **Materials:** in EquipParamWeapon, `materialSetId == level*100` in every row. A step to +n charges EquipMtrlSetParam row n*100, using only the first pair:
  - +1..+3: 3/5/8 Blood Stone Shard (goods 3000)
  - +4..+6: 3/5/8 Twin Blood Stone Shards (3010)
  - +7..+9: 3/5/8 Chunk (3020)
  - +10: 1 Blood Rock (3030)
  - This is the same for every weapon class.
- **Echoes:** each step also costs Echoes (the target row's `basicPrice`, at row +0x18). The refund keeps those Echoes lost, as Asad decided.
- **Item category:** the top 4 bits of an item key: weapon 0, armor 0x1, ring 0x2, goods 0x4, gem 0x8 (<<28).
- **Pointers:** PGD = local PlayerGameData. EGD = PGD+0x1D0 (equipment data). INV = PGD+0x328 (inventory).
- **Inventory entries:** 16 bytes, `{u32 handle; u32 category|id; u32 qty; u32 sort}`. The key-item list is `*(INV+0x58)` (capacity INV+0x24). The normal list follows it in the index space at `*(INV+0x48)` (capacity INV+0x14).
- **Weapon instances:** weapons, armor and gems are instances; the handle has bit 0x800000 set, and `handle&0xFFFF` indexes the manager at `*(base+0x553e990)`.
  - Weapon instance: +0xC id, +0x18 durability.
  - Gem slots s=0..4: shape at +0x38+16s (0x80000000 = closed), gem handle at +0x3C+16s.
  - Fitted gems are not in the inventory list.
- **Gem slots:** they open at +1/+3/+6 (melee) or +1 (firearms). Upgrading opens slots but never closes them, so a downgrade has to close them itself.
- **Equip slots:** 15 of them. The inventory index is at EGD+0x24+4s and the handle at EGD+0x94+4s. Weapons are slots 0..7.

| Function | Offset |
|---|---|
| AddItem(EGD, category, id, qty, 1, bool, [rsp] bool) → index (status at EGD+0x29c: 0 ok, 2 full) | 0x14cdfd0 |
| ChangePlayerItemQty(_, category, id, delta) | 0x131cb70 |
| ChangeQty(EGD, idx, delta, 0, 0, 0) | 0x14d0be0 |
| FindItem(INV, category, id) | 0x14d9e80 |
| INV::AddHandle(INV, &handle, qty, 0, 0) (gem back into the inventory) | 0x14da3f0 |
| ChangeItemId(EGD, idx, newId) (in-place upgrade) | 0x14d1b30 |
| Handle assign | 0x1a8bed0 |
| Gem slot get / set / take | 0x1a941b0 / 0x1a941d0 / 0x1a942a0 |

- **Workshop Fortify execute:** 0x1736140 (FrpgMenuDlgEquipCustomShop confirm, vtable 0x53830c0). It calls ChangeItemId at 0x173636a, then subtracts Echoes, then charges the materials through ChangeQty. The list builder is 0x1f2a760, and the affordability check is 0x1738da0.

## Event flags, the frame hook, sessions and saving
- **Flag manager:** SprjEventFlagMan is the global `0x553b100`.
  - `SetEventFlag(mgr, u32 flag, bool on)` is at `0x13cfcc0`, entry bytes `41 89 d0 8b 4f 1c`. Talk `c1_11` and Lua reach it.
  - `GetEventFlag(mgr, int flag)` is at `0x13bccf0`.
  - Both functions are leaves.
- **Flag storage:** 1000-flag blocks, MSB-first bits, with unknown blocks ignored. Block 72100 is registered as a saved block for m21_00, so 72100900 and 72100901 are backed.
- **Frame hook:** `SprjLuaEventUpdateTask::_STEP_Update` at `0x1393fd0`, bytes `55 48 89 e5 53 50`. It runs once per frame on the game-logic thread, which also runs talk scripts and menus. Gate work on SprjLuaEventMan (`0x553b0c8`) ≠ 0 and PGD ≠ 0.
- **Map packing:** packed map = AA<<24 | BB<<16 | CC<<8 | DD. The Hunter's Dream m21_00_00_00 is `0x15000000`, and m21_01 is `0x15010000`.
- **Session state:** the SprjSessionManager (`0x5540290`) state at +0x124 is 3 for host (`IsHost`) and 6 for client (`IsClient`).
- **Saving:** `SaveRequest(void)` at `0x1336da0` is the Lua handler and needs SprjLuaEventMan.
