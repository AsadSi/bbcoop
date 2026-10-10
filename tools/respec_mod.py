#!/usr/bin/env python3
"""bbcoop: builds the respec data mod: "Reset Attributes" and "Refund Weapon Upgrades" in the Plain
Doll's talk menu (Bloodborne 1.09). The game's files are not in the repository, so the mod is made
from the player's own copy:

    tools/respec_mod.py GAME_DIR MOD_DIR

GAME_DIR is the game folder (with dvdroot_ps4/) or dvdroot_ps4 itself; MOD_DIR gets
dvdroot_ps4/script/talk/m21_00_00_00.talkesdbnd.dcx (Doll = t210304.esd, machine x7, the main talk
menu) and dvdroot_ps4/msg/engus/menu.msgbnd.dcx (entry 30, EventText). The output is re-read and
checked: the new entries exist and everything else is unchanged. run.sh calls this.

Needs soulstruct 2.6 (python3 -m venv ~/.local/share/bbcoop-re; .../bin/pip install soulstruct).
Works around two soulstruct quirks (docs/respec_re.md):
  * ESD.to_writer() keys next-state offsets by state id across all state machines and shares
    conditions file-wide, so in multi-machine talk ESDs (all of Bloodborne's) conditions jump into
    the wrong machine: write_esd() below keeps offsets and condition sharing per machine.
  * BinderEntry.name uses Path(...).name, which does not split Windows paths on Linux: entries are
    found by PureWindowsPath.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path, PureWindowsPath

from soulstruct.containers import Binder
from soulstruct.base.text.fmg import FMG
from soulstruct.bloodborne.ezstate.esd import TalkESD
from soulstruct.base.ezstate.esd.command import Command
from soulstruct.base.ezstate.esd.condition import Condition
from soulstruct.base.ezstate.esd.state import State
from soulstruct.base.ezstate.esd import core as esd_core
from soulstruct.utilities.binary import BinaryReader, BinaryWriter, ByteOrder

# ---------------------------------------------------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------------------------------------------------
GAME = OUT = Path()  # set by main()

TALK_REL = "script/talk/m21_00_00_00.talkesdbnd.dcx"
MSG_REL = "msg/engus/menu.msgbnd.dcx"
DOLL_ESD = "t210304.esd"
WORKSHOP_ESD = "t210692.esd"      # the Workshop tools (the altar); machine x2 state 1 opens Fortify Weapon
WORKSHOP_FORTIFY_SM = 0x80000000 - 2
MENU_SM = 0x80000000 - 7          # "x7" = Doll's main talk menu (Channel Blood Echoes / Talk / Give ... / Leave)
EVENT_TEXT_ENTRY_ID = 30          # menu.msgbnd entry 30 = EventText (talk-list labels 15000xxx, dialogs 1001xxxx)
DIALOG_TEXT_ENTRY_ID = 204        # menu.msgbnd entry 204 = MenuDialogs_SP (the Fortify confirm is 221000)

# Event flags (block 72100xxx: Hunter's Dream NPC flags; 72100500-72100999 unused by every ESD/EMEVD in the game).
FLAG_RESPEC_REQUESTED = 72100900  # ESD sets 1 after "Yes"; native code performs respec, then clears it.
FLAG_REFUND_MODE = 72100901       # 1 while the Doll's fortify menu is open; ESD clears on return to menu.

# Talk-list result indices (existing: 1 Channel, 2 Talk, 3 Give Ornament, 50 Leave).
IDX_RESPEC, IDX_REFUND = 4, 5

# New EventText IDs (all unused in every FMG of menu.msgbnd and item.msgbnd).
TXT_RESPEC_LABEL = 15009100
TXT_REFUND_LABEL = 15009110
TXT_RESPEC_CONFIRM = 10019100
NEW_TEXT = {
    TXT_RESPEC_LABEL: "Reset Attributes",
    TXT_REFUND_LABEL: "Refund Weapon Upgrades",
    TXT_RESPEC_CONFIRM: "Reset all attributes?\nThis costs 1 Insight.",
}
# The Fortify confirm in refund mode (runtime_respec.c swaps 221000 for it; 22109x is unused).
TXT_REFUND_CONFIRM = 221090
NEW_DIALOG_TEXT = {TXT_REFUND_CONFIRM: "Refund this weapon's upgrades?\nIts upgrade materials are returned."}

# New state IDs in machine x7 (vanilla uses 0..30).
S_RESPEC_ASK, S_RESPEC_SET, S_REFUND_SET, S_REFUND_MENU, S_REFUND_DONE = 31, 32, 33, 34, 35

# ---------------------------------------------------------------------------------------------------------------------
# Minimal EZL encoder (same encoding as the game; validated against vanilla bytes in `self_test_ezl`).
# ---------------------------------------------------------------------------------------------------------------------
END = b"\xa1"
CONTINUE_IF_FALSE = b"\xa6"
EQ = b"\x95"


def num(n: int | float) -> bytes:
    if isinstance(n, float):
        return b"\x81" + struct.pack("<d", n)
    if -64 <= n <= 63:
        return bytes([n + 64])
    return b"\x82" + struct.pack("<i", n)


def call(f_id: int, *args: int) -> bytes:
    """Test-function call as the game emits it: id, args, 0x84+argc, then 'continue if false' (0xa6)."""
    return num(f_id) + b"".join(num(a) for a in args) + bytes([0x84 + len(args)]) + CONTINUE_IF_FALSE


def test_eq(f_id: int, args: tuple, value: int) -> bytes:
    return call(f_id, *args) + num(value) + EQ + END


ALWAYS = num(1) + END                       # b'\x41\xa1'
REG0_EQ = lambda v: b"\xaf" + num(v) + EQ + END  # load register 0 (talk-list result stored by first condition)


def cmd(bank: int, index: int, *args) -> Command:
    # Fresh bytearray per arg: soulstruct reserves arg offsets keyed by object id.
    return Command(bank, index, [bytearray(num(a) + END) for a in args])


# Talk command / test IDs used (Bloodborne talk ESD, bank 1). Names are soulstruct's DS1-derived names.
C_ADD_TALK_LIST = 19      # AddTalkListData(index, eventtext_id, required_flag or -1)
C_SET_FLAG = 11           # SetEventFlag(flag, state)
C_GENERIC_DIALOG = 17     # OpenGenericDialog(type, eventtext_id, btn1, btn2, default)  type 2 + (3,4) = Yes/No
C_SET_TALK_TIME = 68      # SetTalkTime(0.33)
C_OPEN_FORTIFY = 48       # opens the Workshop "Fortify Weapon" menu (t210692 x2 State 1); no args
T_DIALOG_OPEN = 21        # IsGenericDialogOpen()
T_DIALOG_RESULT = 22      # GetGenericDialogButtonResult(): 1 = Yes/first button, 2 = No, 0 = closed
T_MENU_OPEN = 59          # CheckSpecificPersonMenuIsOpen(menu_type, 0); fortify menu type = 13


def self_test_ezl(bnd_talk: Binder):
    """Prove the encoder reproduces vanilla bytes for every construct we emit."""
    esd = load_esd(bnd_talk, "t210692.esd")
    s1 = esd.state_machines[0x80000000 - 2][1]   # Workshop: c1_48(); wait f59(13,0)==0
    assert (s1.enter_commands[0].bank, s1.enter_commands[0].index, s1.enter_commands[0].args) == (1, 48, [])
    assert s1.conditions[0].test_ezl == test_eq(T_MENU_OPEN, (13, 0), 0), s1.conditions[0].test_ezl.hex()
    assert bytes(s1.ongoing_commands[0].args[0]) == num(0.33) + END
    doll = load_esd(bnd_talk, DOLL_ESD).state_machines[MENU_SM]
    assert [bytes(a) for a in doll[15].enter_commands[0].args] == [num(6700) + END, num(1) + END]  # SetFlag(6700,1)
    assert doll[15].conditions[0].test_ezl == test_eq(T_DIALOG_OPEN, (), 0)
    assert doll[2].conditions[1].test_ezl == REG0_EQ(1)
    assert [bytes(a) for a in doll[5].enter_commands[3].args] == [num(50) + END, num(15000005) + END, num(-1) + END]
    yn = load_esd(Binder.from_path(GAME / "script/talk/m29_00_00_00.talkesdbnd.dcx"), "t290117.esd")
    st = yn.state_machines[0x80000000 - 5][1]
    assert [bytes(a) for a in st.enter_commands[0].args] == [num(v) + END for v in (2, 10010713, 3, 4, 2)]
    assert st.conditions[0].test_ezl == test_eq(T_DIALOG_RESULT, (), 1)
    assert st.conditions[1].test_ezl == test_eq(T_DIALOG_OPEN, (), 0)


# ---------------------------------------------------------------------------------------------------------------------
# Fixed ESD writer (per-machine next-state offsets and condition sharing).
# ---------------------------------------------------------------------------------------------------------------------
def write_esd(esd: TalkESD) -> bytes:
    long_v = esd.LONG_VARINTS
    esd_name_length = len(esd.esd_name) + 1 if esd.esd_name else 0
    ext = esd_core.ESDExternalHeaderStruct.object_to_writer(
        esd, byte_order=ByteOrder.LittleEndian, long_varints=long_v, esd_name_length=esd_name_length,
        state_machine_count=len(esd.state_machines), game_version=[esd.VERSION, esd.VERSION],
        **esd_core.EXTERNAL_HEADER_VARINT_ASSERTED[long_v],
    )
    w = esd_core.ESDInternalHeaderStruct64.object_to_writer(
        esd, byte_order=ByteOrder.LittleEndian, long_varints=long_v,
        state_machine_count=len(esd.state_machines), esd_name_offset=esd_core.RESERVED,
        esd_name_length=esd_name_length, footer=[-1, -1],
    )
    for smi, states in esd.state_machines.items():
        esd_core.StateMachineHeaderStruct.object_to_writer(
            states, w, index=smi, states_offset=esd_core.RESERVED, state_count=len(states),
            states_offset_2=esd_core.RESERVED)

    sm_state_offsets: dict[int, dict[int, int]] = {}
    all_states: list[tuple[int, State]] = []
    for smi, states in esd.state_machines.items():
        w.fill_with_position("states_offset", obj=states)
        w.fill_with_position("states_offset_2", obj=states)
        offs = sm_state_offsets[smi] = {}
        for sid, state in states.items():
            offs[state.state_id] = w.position
            state.to_esd_writer(w)
            all_states.append((smi, state))
        if len(states) > 1:
            dummy = list(states.values())[0].copy()
            dummy.to_esd_writer(w)
            all_states.append((smi, dummy))
    ext.fill("state_count", len(all_states), obj=esd)

    cond_offsets: dict[int, dict] = {smi: {} for smi in esd.state_machines}
    fresh = [st.pack_conditions(w, sm_state_offsets[smi], cond_offsets[smi]) for smi, st in all_states]
    ext.fill("condition_count", sum(len(d) for d in cond_offsets.values()), obj=esd)
    ext.fill("command_count", sum(st.pack_commands(w, f) for (_, st), f in zip(all_states, fresh)), obj=esd)
    ext.fill("command_arg_count", sum(st.pack_command_args(w, f) for (_, st), f in zip(all_states, fresh)), obj=esd)
    ext.fill("condition_pointers_offset", w.position, obj=esd)
    recurred = {smi: set() for smi in esd.state_machines}
    ext.fill("condition_pointers_count", sum(
        st.pack_condition_pointers(w, cond_offsets[smi], recurred[smi]) for smi, st in all_states), obj=esd)
    for (_, st), f in zip(all_states, fresh):
        st.pack_condition_test_data(w, f)
    for (_, st), f in zip(all_states, fresh):
        st.pack_command_arg_data(w, f)
    ext.fill("tail_offset", w.position, obj=esd)
    if esd.esd_name:
        if w.position % 2:
            w.pad(1)  # vanilla BB ESDs align the UTF-16 name to 2 bytes
        w.fill_with_position("esd_name_offset", obj=esd)
        w.append(esd.esd_name.encode("utf-16-le") + b"\0\0")
    else:
        w.fill("esd_name_offset", -1, obj=esd)
    for k in ("unk_offset_1", "unk_offset_2", "internal_data_size"):
        ext.fill(k, w.position, obj=esd)
    ext.append(bytes(w))
    return bytes(ext)


def check_next_state_pointers(data: bytes) -> int:
    """Raw check: every condition's next-state pointer lands on a state header inside its OWN state machine.
    Returns number of conditions checked. Raises on violation."""
    r = BinaryReader(data, byte_order=ByteOrder.LittleEndian, long_varints=True)
    hdr = esd_core.ESDExternalHeaderStruct.from_bytes(r)
    r = BinaryReader(r.read(), byte_order=ByteOrder.LittleEndian, long_varints=True)
    ih = esd_core.ESDInternalHeaderStruct64.from_bytes(r)
    sms = [esd_core.StateMachineHeaderStruct.from_bytes(r) for _ in range(hdr.state_machine_count)]
    STATE, COND = 72, 56
    checked = 0

    def check_cond(off, lo, hi, smi, seen):
        nonlocal checked
        if off in seen:
            return
        seen.add(off)
        r.seek(off)
        nso, pco, pcc, sco, scc, teo, tes = struct.unpack("<7q", r.read(COND))
        if nso != -1 and nso > 0:
            if not (lo <= nso < hi and (nso - lo) % STATE == 0):
                raise AssertionError(f"machine {smi}: condition @{off:#x} -> state @{nso:#x} outside [{lo:#x},{hi:#x})")
        checked += 1
        if sco > 0:
            r.seek(sco)
            for so in struct.unpack(f"<{scc}q", r.read(8 * scc)):
                check_cond(so, lo, hi, smi, seen)

    for sm in sms:
        lo = sm.states_offset
        n = sm.state_count + (1 if sm.state_count > 1 else 0)
        hi = lo + n * STATE
        seen = set()
        for i in range(n):
            r.seek(lo + i * STATE)
            sid, cpo, cpc = struct.unpack("<3q", r.read(24))
            if cpo != -1:
                r.seek(cpo)
                for co in struct.unpack(f"<{cpc}q", r.read(8 * cpc)):
                    check_cond(co, lo, hi, sm.index, seen)
    return checked


# ---------------------------------------------------------------------------------------------------------------------
# Edit
# ---------------------------------------------------------------------------------------------------------------------
def find_entry(bnd: Binder, basename: str):
    matches = [e for e in bnd.entries if PureWindowsPath(e.path).name == basename]
    assert len(matches) == 1, (basename, len(matches))
    return matches[0]


def load_esd(bnd: Binder, basename: str) -> TalkESD:
    return TalkESD.from_bytes(find_entry(bnd, basename).get_uncompressed_data())


def edit_doll(esd: TalkESD):
    sm = esd.state_machines[MENU_SM]
    assert max(sm) == 30 and not set(sm) & {S_RESPEC_ASK, S_RESPEC_SET, S_REFUND_SET, S_REFUND_MENU, S_REFUND_DONE}

    # State 1 (menu root, re-entered after every action): clear refund mode so an interrupted refund never leaks into
    # the Workshop's own fortify menu.
    sm[1].enter_commands.append(cmd(1, C_SET_FLAG, FLAG_REFUND_MODE, 0))

    # State 5: add list entries just before "Leave" (index 50, last command).
    enter = sm[5].enter_commands
    assert bytes(enter[-1].args[0]) == num(50) + END
    enter[-1:-1] = [
        cmd(1, C_ADD_TALK_LIST, IDX_RESPEC, TXT_RESPEC_LABEL, -1),
        cmd(1, C_ADD_TALK_LIST, IDX_REFUND, TXT_REFUND_LABEL, -1),
    ]

    # State 2 (wait for list choice): register 0 holds GetTalkListEntryResult() from the first condition.
    sm[2].conditions += [
        Condition(S_RESPEC_ASK, REG0_EQ(IDX_RESPEC)),
        Condition(S_REFUND_SET, REG0_EQ(IDX_REFUND)),
    ]

    # Reset attributes: Yes/No dialog -> Yes sets FLAG_RESPEC_REQUESTED -> back to menu. No/close -> back to menu.
    sm[S_RESPEC_ASK] = State(
        S_RESPEC_ASK,
        conditions=[
            Condition(S_RESPEC_SET, test_eq(T_DIALOG_RESULT, (), 1)),
            Condition(1, test_eq(T_DIALOG_OPEN, (), 0)),
        ],
        enter_commands=[cmd(1, C_GENERIC_DIALOG, 2, TXT_RESPEC_CONFIRM, 3, 4, 2)],
        ongoing_commands=[cmd(1, C_SET_TALK_TIME, 0.33)],
    )
    sm[S_RESPEC_SET] = State(
        S_RESPEC_SET,
        conditions=[Condition(1, ALWAYS)],
        enter_commands=[cmd(1, C_SET_FLAG, FLAG_RESPEC_REQUESTED, 1)],
    )

    # Refund weapon upgrades: set refund flag -> open the Workshop fortify menu -> wait until closed -> clear flag.
    sm[S_REFUND_SET] = State(
        S_REFUND_SET,
        conditions=[Condition(S_REFUND_MENU, ALWAYS)],
        enter_commands=[cmd(1, C_SET_FLAG, FLAG_REFUND_MODE, 1)],
    )
    sm[S_REFUND_MENU] = State(
        S_REFUND_MENU,
        conditions=[Condition(S_REFUND_DONE, test_eq(T_MENU_OPEN, (13, 0), 0))],
        enter_commands=[cmd(1, C_OPEN_FORTIFY)],
        ongoing_commands=[cmd(1, C_SET_TALK_TIME, 0.33)],
    )
    sm[S_REFUND_DONE] = State(
        S_REFUND_DONE,
        conditions=[Condition(1, ALWAYS)],
        enter_commands=[cmd(1, C_SET_FLAG, FLAG_REFUND_MODE, 0)],
    )


def edit_workshop(esd: TalkESD):
    """The altar's Fortify Weapon switches refund mode off first: the native refund mode follows the
    Doll's writes of the flag, and a talk interrupted with her menu open would otherwise leave it on."""
    state = esd.state_machines[WORKSHOP_FORTIFY_SM][1]
    assert [(c.bank, c.index) for c in state.enter_commands] == [(1, C_OPEN_FORTIFY)]
    state.enter_commands.insert(0, cmd(1, C_SET_FLAG, FLAG_REFUND_MODE, 0))


# ---------------------------------------------------------------------------------------------------------------------
# Comparison helpers
# ---------------------------------------------------------------------------------------------------------------------
def cmd_key(c: Command):
    return (c.bank, c.index, tuple(bytes(a) for a in c.args))


def cond_key(c: Condition):
    return (c.next_state_id, bytes(c.test_ezl), tuple(cmd_key(p) for p in c.pass_commands),
            tuple(cond_key(s) for s in c.subconditions))


def state_key(s: State):
    return (s.state_id, tuple(cond_key(c) for c in s.conditions), tuple(cmd_key(c) for c in s.enter_commands),
            tuple(cmd_key(c) for c in s.exit_commands), tuple(cmd_key(c) for c in s.ongoing_commands))


def esd_key(e: TalkESD):
    return (tuple(e.magic), e.esd_name,
            {smi: {sid: state_key(s) for sid, s in st.items()} for smi, st in e.state_machines.items()})


def main():
    global GAME, OUT
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    game = Path(sys.argv[1])
    GAME = game if game.name == "dvdroot_ps4" else game / "dvdroot_ps4"
    OUT = Path(sys.argv[2]) / "dvdroot_ps4"
    talk_src, msg_src = GAME / TALK_REL, GAME / MSG_REL
    talk = Binder.from_path(talk_src)
    msg = Binder.from_path(msg_src)

    self_test_ezl(talk)

    # --- 0. our writer round-trips the vanilla Doll ---
    vanilla_doll = load_esd(talk, DOLL_ESD)
    check_next_state_pointers(find_entry(talk, DOLL_ESD).get_uncompressed_data())
    rt = write_esd(vanilla_doll)
    check_next_state_pointers(rt)
    assert esd_key(TalkESD.from_bytes(rt)) == esd_key(vanilla_doll)

    # --- 1. talk ESD ---
    doll = load_esd(talk, DOLL_ESD)
    edit_doll(doll)
    doll_entry = find_entry(talk, DOLL_ESD)
    doll_entry.set_uncompressed_data(write_esd(doll))
    workshop = load_esd(talk, WORKSHOP_ESD)
    edit_workshop(workshop)
    find_entry(talk, WORKSHOP_ESD).set_uncompressed_data(write_esd(workshop))
    talk_out = OUT / TALK_REL
    talk_out.parent.mkdir(parents=True, exist_ok=True)
    talk_out.write_bytes(bytes(talk))

    # --- 2. EventText FMG ---
    fmg_entry = [e for e in msg.entries if e.entry_id == EVENT_TEXT_ENTRY_ID][0]
    fmg = FMG.from_bytes(fmg_entry.get_uncompressed_data())
    item = Binder.from_path(GAME / "msg/engus/item.msgbnd.dcx")
    for b in (msg, item):
        for e in b.entries:
            ids = FMG.from_bytes(e.get_uncompressed_data()).entries
            clash = (set(NEW_TEXT) | set(NEW_DIALOG_TEXT)) & set(ids)
            assert not clash, (e.path, clash)
    fmg.entries.update(NEW_TEXT)
    fmg_entry.set_uncompressed_data(bytes(fmg.to_writer()))
    dialog_entry = [e for e in msg.entries if e.entry_id == DIALOG_TEXT_ENTRY_ID][0]
    dialog = FMG.from_bytes(dialog_entry.get_uncompressed_data())
    assert not set(NEW_DIALOG_TEXT) & set(dialog.entries)
    dialog.entries.update(NEW_DIALOG_TEXT)
    dialog_entry.set_uncompressed_data(bytes(dialog.to_writer()))
    msg_out = OUT / MSG_REL
    msg_out.parent.mkdir(parents=True, exist_ok=True)
    msg_out.write_bytes(bytes(msg))

    verify(talk_src, talk_out, msg_src, msg_out)
    print(f"wrote {talk_out}\nwrote {msg_out}")


def verify(talk_src, talk_out, msg_src, msg_out):
    from soulstruct.dcx import decompress

    # Containers: same DCX type, same binder header, same entry list (ids, paths, flags, order).
    for src, out in ((talk_src, talk_out), (msg_src, msg_out)):
        a, b = Binder.from_path(src), Binder.from_path(out)
        assert a.dcx_type == b.dcx_type, (a.dcx_type, b.dcx_type)
        assert (a.version, a.v4_info, a.flags, a.signature, a.big_endian, a.bit_big_endian) == \
               (b.version, b.v4_info, b.flags, b.signature, b.big_endian, b.bit_big_endian)
        assert [(e.entry_id, e.path, e.flags) for e in a.entries] == [(e.entry_id, e.path, e.flags) for e in b.entries]
        da = decompress(src.read_bytes())[0]
        db = decompress(out.read_bytes())[0]
        print(f"{out.name}: dcx={b.dcx_type.name} raw {len(da)} -> {len(db)} bytes, {len(b.entries)} entries")

    # Talk: every entry except the Doll is byte-identical; Doll differs only by the intended edit.
    a, b = Binder.from_path(talk_src), Binder.from_path(talk_out)
    changed = [PureWindowsPath(x.path).name for x, y in zip(a.entries, b.entries)
               if x.get_uncompressed_data() != y.get_uncompressed_data()]
    assert sorted(changed) == sorted([DOLL_ESD, WORKSHOP_ESD]), changed
    new_workshop = find_entry(b, WORKSHOP_ESD).get_uncompressed_data()
    check_next_state_pointers(new_workshop)
    expected_workshop = load_esd(a, WORKSHOP_ESD)
    edit_workshop(expected_workshop)
    assert esd_key(TalkESD.from_bytes(new_workshop)) == esd_key(expected_workshop)
    new_bytes = find_entry(b, DOLL_ESD).get_uncompressed_data()
    check_next_state_pointers(new_bytes)
    new = TalkESD.from_bytes(new_bytes)
    expected = load_esd(a, DOLL_ESD)
    edit_doll(expected)
    assert esd_key(new) == esd_key(expected)
    old = load_esd(a, DOLL_ESD)
    ko, kn = esd_key(old)[2], esd_key(new)[2]
    assert set(ko) == set(kn)
    for smi in ko:
        if smi != MENU_SM:
            assert ko[smi] == kn[smi], smi
    diff_states = sorted(s for s in set(ko[MENU_SM]) | set(kn[MENU_SM]) if ko[MENU_SM].get(s) != kn[MENU_SM].get(s))
    assert diff_states == [1, 2, 5, 31, 32, 33, 34, 35], diff_states
    sm = new.state_machines[MENU_SM]
    adds = [(int(cmd_key(c)[2][1][1:5].hex() and struct.unpack('<i', cmd_key(c)[2][1][1:5])[0]))
            for c in sm[5].enter_commands if c.index == C_ADD_TALK_LIST and cmd_key(c)[2][1][0] == 0x82]
    assert adds == [15000100, 15000007, 15000008, TXT_RESPEC_LABEL, TXT_REFUND_LABEL, 15000005], adds
    print(f"talk OK: only {DOLL_ESD} and {WORKSHOP_ESD} (fortify clears refund mode) changed; machine x7 states changed/added: {diff_states}; menu labels {adds}")

    # Msg: every FMG except EventText and MenuDialogs_SP byte-identical; those two = vanilla + the new strings.
    a, b = Binder.from_path(msg_src), Binder.from_path(msg_out)
    changed = [x.entry_id for x, y in zip(a.entries, b.entries)
               if x.get_uncompressed_data() != y.get_uncompressed_data()]
    assert changed == sorted([EVENT_TEXT_ENTRY_ID, DIALOG_TEXT_ENTRY_ID]), changed
    norm = lambda d: {k: (v or "") for k, v in d.items()}
    for entry_id, new_text in ((EVENT_TEXT_ENTRY_ID, NEW_TEXT), (DIALOG_TEXT_ENTRY_ID, NEW_DIALOG_TEXT)):
        fa = FMG.from_bytes([e for e in a.entries if e.entry_id == entry_id][0].get_uncompressed_data())
        fb = FMG.from_bytes([e for e in b.entries if e.entry_id == entry_id][0].get_uncompressed_data())
        ea, eb = norm(fa.entries), norm(fb.entries)
        assert {k: v for k, v in eb.items() if k not in new_text} == ea
        assert {k: eb[k] for k in new_text} == new_text
        print(f"msg OK: entry {entry_id}: {len(ea)} -> {len(eb)} strings; new: {new_text}")

if __name__ == "__main__":
    main()
