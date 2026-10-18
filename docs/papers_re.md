# Fire Paper, Bolt Paper, Empty Phantasm Shell: reverse engineering (v1.09)

Asad asked for this on 2026-10-17/18: the papers should keep a flat bonus for the early game, add a share of the weapon's
damage for the late game, and not favour fast weapons. The rule he chose:

    bonus = 80 x (base at +0 x multiplier) / 90  +  10% x (base at the current level x multiplier)

- `base` is the weapon's EquipParamWeapon `attackBasePhysics` (the +0 row for the first part, the weapon's own row for
  the second).
- `multiplier` is the attack's AtkParam_Pc `atkPhysCorrection` / 100 (the motion value).
  - An attack with no physical multiplier uses the item's own element's correction (`atkFireCorrection` for Fire
    Paper; e.g. the Holy Moonlight Sword's beams, 180–280%).
  - An attack with no multiplier at all (a stamina-only guard break) gets 0.
  - A weapon with base 0 at +0 (Kos Parasite) keeps the vanilla +80. Papers can't go on elemental weapons anyway.
- 90 is the Saw Cleaver's base, so its R1 (100%) keeps the item's +80 at +0.
- Stat scaling and Blood gems don't change the bonus (Asad: "before scaling", so it doesn't grow too strong).
- `src/runtime_papers.c` does it, `tests/test_papers.c` tests it, and `BB_PAPERS=vanilla` (or `0`, `off`, `false`)
  turns it off. `BB_PAPERS_TRACE=1` logs the first bonus of each (paper, weapon, attack), up to 256 of them, with its
  old and new value.

All addresses are image offsets. File offset = image offset + 0x4000 in `out/eboot.elf`. The work was done by reading the
disassembly (objdump plus a capstone per-function listing, bounds from `.eh_frame_hdr`) and the game's own params. No
game runs were made.

## The items in the params
| Item (goods) | SpEffectParam | Field | stateInfo (weapon effect) |
|---|---|---|---|
| Fire Paper (1300) | 2200 | fireAttackPower 80 | 62 |
| Bolt Paper (1320) | 2220 | thunderAttackPower 80 | 61 |
| Empty Phantasm Shell (1310) | 2210 | magicAttackPower 80 | 60 |

- Each lasts 60 s and has no stat-scaling flag (`bAdjustMagicAblity` and similar are 0), so the vanilla +80 doesn't grow
  with Arcane.
- SpEffectParam offsets: physics/magic/fire/thunder AttackPower at 0x58/0x5C/0x60/0x64 (s32), their rates at 0x48..0x54,
  stateInfo at 0x156 (u16), wepParamChange at 0x158, throw/charge AttackParamChange at 0x164 bits 6/5.

## Where the game sums effects for an attack: 0x17e5a30
- Arguments:
  - rdi: an effect owner; [rdi+8] is the list of active effect instances.
  - rsi: the attack's struct (kept in rbx).
  - edx: a mask (0xf from most callers).
- Instance:
  - +0x48: the SpEffectParam row.
  - +0x58: the next instance.
  - +0x1c: flags (0x800c0003 skips).
  - +0x38: the target mask.
  - +0xc..+0x18: four floats. Every positive flat add is multiplied by their product.
- Per instance, the condition 0x17eae90 gets the attack's behavior category, its throw flag (+0xbc) and its charge flag
  (+0xbe). It switches on the category against the effect's `wepParamChange` (0 for the papers, so every category
  passes there), then honours the effect's throw/charge-only bits.
- **Category 5** (struct +0x14 == 5): only effects with stateInfo 123–126 or 186 count, through the path at 0x17e5e40.
  The papers (60–62) never do.
  - Category 5 is a block of 31 generic BehaviorParam_PC rows (1, 10, 404–602), shared by every weapon and costing no
    stamina.
  - That fits Asad's source that visceral attacks get no paper bonus. I did not confirm that the visceral animation
    events use those rows (the TAE wasn't read).
  - The hook only changes a value the game has already decided to add, so whatever vanilla excludes stays excluded.
- **Normal path:** 0x17e5d95 → 0x17e611c. Each add is computed as `row int x the product of the four floats` and
  then added to the totals:
  - 0x17e61de: +magic into xmm4.
  - 0x17e6210: +fire into xmm3.
  - 0x17e6242: +bolt into xmm2.
  - After the site: 0x17e628d / 0x17e62a1 / 0x17e62b2 add them to the totals at rbp-0x94 / -0x90 / -0x80.
- At the end, the totals go to the attack struct: +0x20.. (physical, slash, blow, thrust), +0x30 (neutral), +0x34
  (magic), +0x38 (fire), +0x3c (bolt); the rates follow at +0x40...
- **The hook** sits at 0x17e626e (`vmovaps xmm5, [rbp-0x60]`, 5 bytes), just after the three loads.
  - There, r14 is the effect's row, rbx the attack struct, and xmm4/xmm3/xmm2 hold this effect's adds.
  - For a paper's row, it multiplies the matching register by `bonus / 80`, which keeps the four factors.

## The attack struct
- It is built by 0x1576090 for real hits; the visceral path 0x1a29890 → 0x18652c0 also builds it.
- Fields:
  - +0: the character.
  - +8: AtkParam id (the BehaviorParam row's refId).
  - +0xc: the AtkParam table selector.
  - +0x14: the BehaviorParam category.
  - +0x18: flags.
  - +0x84: the weapon id (1000000, bare fists, when flags & 0x60).
  - +0xbc: throw (visceral).
  - +0xbd: an unknown flag; when set, the weapon field is 3000.
  - +0xbe: charge.
- **Table selector:** 0x1f12fd0 looks up param table 7 + selector. Selector 1 is AtkParam_Pc. Evidence: the
  attack-display caller 0x17caa70 builds a struct with selector 1, AtkParam 101 and the weapon from the player's
  equipment, and the Dark Souls param order puts AtkParam_Npc at 7 and AtkParam_Pc at 8.
  The hook only acts on selector 1.
- **Callers of 0x17e5a30:**
  - 0x15761e4: the builder above.
  - 0x186516d.
  - 0x17cb8eb and 0x1627ba5: AtkParam 101, which has a 100% multiplier, so they look like the attack display.
    0x1627ba5 sets the weapon to −1, so the hook leaves it vanilla.
  - 0x18fd00d: mask 1.
  - 0x19ee959.
- **Lookups the hook calls (pure binary searches over the loaded params, manager at 0x5540340):**
  - 0x1f28aa0(out, id) for SpEffectParam (table 15).
  - 0x1f12fd0(out, selector, id) for AtkParam.
  - 0x1f29aa0(out, id) for EquipParamWeapon (table 0). It rounds the id down to 100 and also fills a
    ReinforceParamWeapon lookup at out+0x18.
  - Each writes the row pointer at out+8.

## The damage side: 0x1576df0
- It reads the attack struct from a stack argument (r12).
- Its AtkParam row's corrections multiply the weapon's parts: `atkPhysCorrection` at 0x1577baa, `atkFireCorrection`
  at 0x157868b.
- Each element's total from the struct is added at the very end, outside the multiplier. For fire:
  - `... x [rbp-0x94] x [struct+0x58] + [struct+0x38]` at 0x1577851 (the display form, without the attack multiplier).
  - The same at 0x1578898 after the multiplier (xmm3 = atkFireCorrection / 100).
- So the vanilla +80 is flat after the motion value, as Asad's source says. The new value takes its place at the same
  point.
- When the struct's throw flag (+0xbc) is set, the weapon's `throwAtkRate` applies: (rate + 100) x a constant at
  0x1576fa2.

## Review (2026-10-18, a second pass over the disassembly)
- Confirmed:
  - Nothing between 0x17e611c and the site writes rbx or r14.
  - The lanes go to the totals at struct +0x34/+0x38/+0x3c (epilogue 0x17e73a8–0x17e73cf).
  - The category-5 path can't carry stateInfo 60–62.
  - r14 is the param row itself: instance+0x40 is a 0x1f28aa0 out struct (0x17e37f6), so comparing pointers works.
  - Selector 1 is AtkParam_Pc: the eboot's SoloParamRepository name list has AtkParam_Npc at 7 and AtkParam_Pc at 8.
  - Every caller fills +8, +0xc and +0x84:
    - 0x186516d sets the weapon or −1.
    - 0x18fd00d sets it (its fallbacks 2199000 and 2099000 have no row).
    - The bullet path 0x19ee959 starts from −1 (constructor 0x19edc20; a second constructor, caller 0x19ef2ae, is unchecked).
  - The lookups write at most 0x28 bytes.
  - Every weapon id has `id % 10000` in 0..1000 in steps of 100.
- Open:
  - The second display caller (0x1627ba5) passes weapon −1, so a menu built on it would still show +80.
  - NPC hunters that use papers, with AtkParam_Pc attacks and a real weapon, would get the new formula too.
  - Check both with the trace.

## Verified in game (Asad, 2026-10-18)
- The test used the Empty Phantasm Shell with `BB_PAPERS_TRACE=1`. Log: `out/play-ab-papers-20261018.log`.
- Ludwig's Holy Blade +10 (8101000, base 100 at +0 and 200 now). Every bonus matched the formula:
  - sword R1 810000 (70%): +76.2
  - 810400/410/430 (100%): +108.9
  - 810420 (106%): +115.4
  - sword charged R2 810170 (120%): +130.7
  - transformed charged R2 810930 (220%): +239.6
- Threaded Cane +10 (22001000): +84.9.
- The status screen uses attack 101 (100%) and shows the new value.
- Viscerals, done with and without the Shell, produced no trace line, so they get no bonus.
- Against `BB_PAPERS=vanilla`, the transformed R2 did a little more damage with the new formula. That fits an
  uncharged R2 (+131 instead of +80 arcane before the enemy's arcane defence). Asad accepted it.

## Was open before the test
- That a paper's bonus in game matches the formula (`BB_PAPERS_TRACE=1` logs it).
- That the status screen's attack display shows the new value: it uses AtkParam 101 (100%), so it would show
  80 x base/90 + 10% of the current base.
- That visceral attacks stay without the bonus.
