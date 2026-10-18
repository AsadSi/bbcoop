#!/usr/bin/env bash
# bbcoop: tools/new_game.sh [--continue] [run.sh args]: starts the game in a throwaway profile
# (BB_USER_DIR, default out/test-user; the real saves in user/ are never touched) and drives it into
# Iosefka's Clinic with scripted input (BB_PAD_FILE=out/pad, tools/press.sh), for testing.
#   new game:   title (Play Offline), New Game, brightness and controls (Next), the intro skipped
#               (Options), contract (name BB_IME_TEXT, default Hunter; Finish; Yes), the
#               transfusion skipped. About 1.5 minutes. Then the profile is kept as
#               out/test-user-clinic for --continue.
#   --continue: copies out/test-user-clinic back and loads it (title, Play Offline, Continue).
# Ends when frame stats report a level (more than 600 draws per frame, as tools/restart.sh) and
# prints the last frame stats line; the game keeps running. The log is out/session.log.
cd -- "$(dirname -- "$0")/.."
mode=new
[[ ${1:-} == --continue ]] && { mode=continue; shift; }
user=${BB_USER_DIR:-$PWD/out/test-user}
snapshot=${BB_SNAPSHOT:-$PWD/out/test-user-clinic} # bbcoop: BB_SNAPSHOT, another saved profile for --continue
case $user in "$PWD"/out/*) ;; *) echo "BB_USER_DIR must be under out/ (it is deleted)" >&2; exit 1;; esac
if [[ $mode == continue && ! -d $snapshot ]]; then echo "No $snapshot: run tools/new_game.sh first." >&2; exit 1; fi

# bbcoop: only this profile's game; other worktrees' and the player's games are left alone.
own="bb-probe.*--user $user( |$)"
pkill -f "$own"; sleep 2; pkill -9 -f "$own"; sleep 1
rm -rf -- "$user"
[[ $mode == continue ]] && cp -a "$snapshot" "$user"
: > out/pad
# bbcoop: test runs skip the startup logos (the skip_intro setting), in a copy of the player's
# settings: menu changes made in a test stay out of the real bbport.ini. BB_CONFIG overrides.
if [[ -z ${BB_CONFIG:-} ]]; then
    { [[ -f bbport.ini ]] && cat bbport.ini; echo skip_intro=1; } > out/test-bbport.ini
    export BB_CONFIG=$PWD/out/test-bbport.ini
fi
# The scripted input below expects the online/offline choice (upstream 0.5 skips it by default).
# bbcoop: macOS has neither setsid nor stdbuf; bb-probe writes its output unbuffered anyway.
detach=(setsid stdbuf -oL -eL)
[[ $(uname -s) == Darwin ]] && detach=()
BB_SKIP_NETWORK_CHOICE=${BB_SKIP_NETWORK_CHOICE:-0} \
BB_USER_DIR=$user BB_IME_TEXT=${BB_IME_TEXT:-Hunter} BB_PAD_FILE=$PWD/out/pad BB_FRAME_STATS=1 \
    ${detach[@]+"${detach[@]}"} bash run.sh "$@" > out/session.log 2>&1 < /dev/null &

press() { tools/press.sh "$1" 0.15; sleep "${2:-1}"; }
in_level() {
    local last
    last=$(grep '^Frame stats' out/session.log | tail -1 | sed -E 's/.* ([0-9]+) draws\/frame.*/\1/')
    [[ -n $last && $last -gt 600 ]]
}
alive() { pgrep -f "$own" > /dev/null || { echo "The game exited; see out/session.log." >&2; exit 1; }; }

# Title screen: the pad is open and the logos have played.
for i in $(seq 1 180); do sleep 1; grep -q 'pad opened' out/session.log && break; done
sleep "${TITLE_WAIT:-25}"; alive
mounts() { grep -c "save data 'SPRJ0005' mounted" out/session.log; }
before=$(mounts)
press down; press cross 1          # Play Offline: the save is read before the notice shows
for i in $(seq 1 30); do [[ $(mounts) -gt $before ]] && break; sleep 1; done
sleep 4
before=$(mounts)
press cross 1                      # "Starting game in offline mode": OK; the save is read again
for i in $(seq 1 30); do [[ $(mounts) -gt $before ]] && break; sleep 1; done
sleep 5                            # the main menu takes input a few seconds later
if [[ $mode == new ]]; then
    press cross 5                  # New Game
    press down; press cross 4      # Brightness: Next
    for i in 1 2 3 4 5 6; do press down; done
    press cross 8                  # Controls: Next; the intro starts
    press options 6                # skip the intro; the contract opens
    press cross 3                  # Enter Name (BB_IME_TEXT)
    press up; press cross 2        # Finish
    press left; press cross 25     # "Finalize contract?": Yes; loading, the transfusion starts
    press options 3                # skip it
else
    # Continue; the first press on the menu is sometimes taken before the menu is ready, so press
    # again until the level starts loading (the character threads are created).
    for i in 1 2 3 4; do
        press cross 8
        grep -q "guest thread 'CSChrThread0'" out/session.log && break
    done
fi
for i in $(seq 1 90); do sleep 2; alive; in_level && break; done
in_level || { echo "Not in a level after 3 minutes; see out/session.log." >&2; exit 1; }
if [[ $mode == new ]]; then
    sleep 15                       # the autosave after arriving
    rm -rf -- "$snapshot"; cp -a "$user" "$snapshot"
    echo "Saved the clinic profile as $snapshot (tools/new_game.sh --continue)."
fi
grep '^Frame stats' out/session.log | tail -1
