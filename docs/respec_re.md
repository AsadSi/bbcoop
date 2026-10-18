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
- **Who's who:** t210304 is the Plain Doll (`c8030_0000`), and t210692 is the Workshop's workbench (machine x2: Fortify Weapon, Repair Weapons, Blood Gem Fortification, Arcane Haze, Leave; list built in state 6, choice in state 3, menu root state 2, Fortify state 1).
- **Where:** "Reset Attributes" is in the Doll's menu (machine x7), "Refund Weapon Upgrades" in the workbench's, just before Leave (states 27–29: flag 72100901 on, `c1_48`, wait for the menu to close, flag off; the menu root and Fortify Weapon switch it off too). It was at the Doll until 2026-10-12.
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
  - Upgrade path: ChangeItemId → 0x14db020 builds a temporary instance from WeaponGenParam[new id] (0x1a876d0) and 0x1a94340 copies each of its open slots into a slot of the weapon that is still closed (0x80000000).
  - Gun gems (`tools/respec_mod.py`, off with `BB_GUN_GEMS=0`): firearms are the `leftHandEquipable` weapons that can be upgraded (have a +1 row), which leaves out the shields and the plain Torch (+0 only). Cannon, Rosmarinus, Flamesprayer, Fist of Gratia, Church Cannon and the Hunter's Torch +1..+10 get the Hunter Pistol's row (one droplet slot, type 0x8). Since 2026-10-17 (Asad's choice) that slot is a Triangle (0x2) on the Flamesprayer, Rosmarinus and Hunter's Torch and a Radial (0x1) on the Fist of Gratia (`GUN_GEM_SHAPES`); the Cannon and Church Cannon keep the Droplet. A weapon instance keeps the shape its slot opened with (+0x38), so one that already has the Droplet slot keeps it until a refund and a new upgrade. Every GemCategoryParam row enables all slot types, and no EquipParamWeapon field separates them from the slotted guns. A gun gets the slot at its next upgrade (or as a new instance); one already at +10 needs a refund at the workbench and a new upgrade.
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

- **FrpgMenuDlgEquipCustomShop is dead code:** 0x1736140, 0x1737cd0 and 0x1738ed0 are never reached; hooks there never fired in game.

## The live Fortify menu ("WeaponUpgrade")
The Workshop altar and the Doll's `c1_48` open it. It is registered at 0x1bb9d00.

**List source and filters**
- The list source is 0x1c75fc0, with a per-tab filter for Right and Left Hand: 0x1c688f0 and 0x1c68a50.
- Each filter lists a weapon only when the row `id + 100` exists, so +10 weapons are missing from the list.
- The row lookup happens right after the hooked sites 0x1c689e1 and 0x1c68b41 (`mov [rbp-0x60],-1`). There, eax is the id and esi is the row to look up.

**Per-item setup** (0x1bd2d40, item `I`)
- `I` holds: +0x48 inventory index, +0x4C item key, +0x10 echo cost, +0x14 Insight cost, +0x20 materials.
- The cost comes from 0x1b5eec0 (the next row's `basicPrice` plus the first material of its set).
- At the hooked site 0x1bd2d73, the cost is on the stack at rbp-0x60, and the material vector's begin and end are at rbp-0x48 and rbp-0x40.

**Target**
- 0x1c5e500 computes `(id & 0xFFFFFFF) + 100`.
- At the hooked site 0x1c5e54d, ebx holds that target, before the row-exists check.
- The preview, the details panel and the execute all use it.

**Confirm**
- 0x1bd2fd0 builds two callbacks: PayCost (0x1b63bc0, which takes I+0x10 echoes, I+0x14 Insight and the materials from inventory then storage) and the upgrade effect (0x1b5dd30).
- 0x1b5dd30 recomputes the target, then calls `ChangeItemId(EGD, I+0x48, target)` at 0x1b5dd8f.
- The hooked site 0x1b5dd88 sits just before that call: r14 = I, rbx = EGD, edx = the target.
- Afterwards the player is refreshed and the game saves.

**How refund mode uses these hooks**
- The filters look up the +0 row for upgraded weapons and row 0x63 (none) for everything else.
- The cost is set to 0.
- The target is set to +0.
- Before ChangeItemId: the gems go to the inventory or storage, the slots are closed, and the materials are returned through the game's AddItemOrStorage (0x1b5c8c0). If anything fails, edx is set back to the weapon's own id.

**Refund-mode cosmetics**
- The info panel's cost is computed separately, in 0x1c5e500 → 0x1b5eec0. At the hooked site 0x1c5e82b (r15 = I) it sits at rbp-0x150, in the same layout as at 0x1bd2d73, and refund mode blanks it.
- The confirm lists what comes back (2026-10-12, verified in game by Asad): `tools/respec_mod.py` adds 221091..221100, one per level +1..+10, with the game's goods names and EquipMtrlSetParam counts (checked against the refund table). The target hook remembers the highlighted weapon's item key (the menu item it sees is a temporary copy, gone when the confirm opens), and the text hook picks 221090 + its level; 221090 is the fallback without a list.
- The confirm text is MenuDialogs_SP (FMG 204) id 221000, "Use <?demandSoul?> Blood Echoes and …". The menu text lookup 0x1ae8cc0(out, fmg, id) is hooked after its pushes at 0x1ae8cd1. That hook turns it into 221090, which `tools/respec_mod.py` adds: "Refund this weapon's upgrades?".

**Gems in the test character**
- AddItem can't make a gem, because a gem is generated.
- `0x1a88130(&handle, *(base+0x553e990), &none, &none, base+0x555235c, gemGenId)`, where `none = {u32 -1, 0, NULL}`, generates one. GemGenParam 110001 is a Radial rough.
- Fitting it is `HandleAssign(&weapon[0x3C+16s], &handle)` followed by a release of the local reference (0x1a8bc00).
- Inventory key of a gem: 0x80000000 | the generator id.

## Stat reset gotcha
The level-up commit (0x1c13d00) changes current HP by the change in max HP, clamped at 0. Lowering Vitality from 50 left 0 HP, and the character died and dropped the refunded echoes. So HP is filled to the new maximum afterwards: live data `[[chr+0x3B0]+0x20]` +0xF8/+0xFC, and PGD +0x14/+0x18.

## Testing notes
- `BB_TEST_ENDGAME=1` builds the test character as follows:
  - Weapons are given at +0, because the give path (0x131cb70) drops the upgrade level, and then upgraded through ChangeItemId.
  - Doll and Dream flags: 9400–9404 and 12417810, plus the tutorial flags.
  - It warps with `0x13CDF30(2102950)`, the Dream's lamp arrival.
- The Doll wakes when the Dream loads with flag 9401 set and Insight ≥ 1.
- Her first talk plays an intro that ends in Level Up.
- Talk prompt: walk to her and press Cross. The menu appears after her greeting line, about 3 s later.
- The Workshop's Fortify, Repair and Blood Gem menus are at the blue-lit workbench with a lantern, right of the Workshop entrance, by the fireplace. They are not at the candle altar at the back, which is the rune altar.
- To reach the workbench with scripted input from the Dream's lamp arrival: talk to the Doll (forward-right), go up the stairs behind her, enter the door on the left, walk past Gehrman toward the fireplace, then step back from the bench and walk into its front (`lx=255`), and press Cross.
- In the Fortify list, the d-pad left/right switches between right-hand and left-hand weapons.
- `BB_TEST_ENDGAME` also gives every firearm that gets a slot from the gun gems (Cannon +1; Flamesprayer, Rosmarinus, Hunter's Torch, Fist of Gratia and Church Cannon +10), Quicksilver Bullets and Blood Vials, and logs whether each gun's first gem slot is open.
- It also puts 12 loose gems in the inventory, from GemGenParam rows with one fixed shape and fixed effects: 5 Droplet (the guns' slot shape), 3 Radial, 2 Triangle and 2 Waning. Each is generated, added with AddHandle and released, as for a refunded gem.
- **Slot shapes** (GemGenParam slotTypeRateA..F = slot types 1 << 0..5): A Radial, B Triangle, C Waning, D (0x8) Droplet. The Petrified Droplet gems (goods 1678-1683) give rows 110031-110037 (JP 円型). The English description reads "Droplet blood gem. Fortifies any weapon".
- `SprjLuaEventUpdateTask::_STEP_Update` (0x1393fd0) never runs in this game. The per-update hook is inside SosStatus.Update (0x187236d), which runs offline too.

## The TEST character (BB_TEST_LOADOUT, 2026-10-12)
- **Name:** PlayerGameData +0xA8 holds the character's name, UTF-16 (found in userdata0000: the player data block there has
  the same layout as in memory: HP +0x14, stats +0x40.., Insight +0x84, level +0x90, Echoes +0x94).
- **What it does:** a character named TEST (any case) is filled once, flag 72100903. It backs up the saves, sets all stats to
  99 (level 544), 99,999,999 Echoes and 99 Insight, then gives the list.
  - Weapons are given at +0 and changed to their final level, as the Workshop does.
  - Armor uses category 0x10000000, goods 0x40000000.
  - It adds the 12 test gems, sets the Dream flags and warps to the Dream (as BB_TEST_ENDGAME).
  - It gives the three Workshop tools (4102, 4103, 4104; key items) under their own flag 72100904, so a TEST filled
    before they were added gets them on its next load. The workbench's talk script sets 72100200/72100201 (Blood Gem
    Fortification, Arcane Haze) itself when it finds them (t210692 x2 states 7/8 and 19/20).
- **The list:** `tools/respec_mod.py` writes it from the game's params and names:
  - EquipParamWeapon rows `id % 10000 == 0` with a name, except Bare Fists, at their highest level;
  - EquipParamProtector rows with a name;
  - EquipParamGoods with goodsType 0, 2 or 6 (not 1, the key items), a name and maxNum > 0, gestures 800–899 left out,
    at maxNum.

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
