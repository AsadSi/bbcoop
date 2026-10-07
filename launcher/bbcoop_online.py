# SPDX-License-Identifier: GPL-2.0-or-later
"""bbcoop: online co-op for the launcher, without any GTK.

Off, Host or Join. The host's launcher runs the shadNet server (server/, built by
scripts/build-server.sh) next to the game and shows the address and invite code to share; a
friend enters both, and their game creates its account with the code on first contact. Friends
connect over Tailscale (or a LAN), so the address is the host's Tailscale IP when there is one.
"""

import secrets
import shutil
import socket
import subprocess
import time
from pathlib import Path

MODES = ("off", "host", "join")
GAME_PORT, STUN_PORT, WEB_PORT = 31313, 31314, 31315


def new_secret():
    """A random code people can read out loud: invite codes and per-install passwords."""
    alphabet = "abcdefghjkmnpqrstuvwxyz23456789"  # no 0/o, 1/l/i
    return "-".join("".join(secrets.choice(alphabet) for _ in range(4)) for _ in range(3))


def ensure_secrets(settings):
    """Fills the settings' invite code and password on first use; returns True when it did."""
    changed = False
    for key in ("coop_invite", "coop_password"):
        if not settings.get(key):
            settings[key] = new_secret()
            changed = True
    return changed


def share_address():
    """The address friends should use: the Tailscale IPv4 when Tailscale is up, else the LAN one."""
    try:
        run = subprocess.run(["tailscale", "ip", "-4"], capture_output=True, text=True, timeout=5)
        if run.returncode == 0 and run.stdout.split():
            return run.stdout.split()[0]
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(("10.255.255.255", 1))  # no packet is sent; picks the outgoing interface
            return probe.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def environment(settings):
    """Environment variables for run.sh. Off sets nothing: the network stays unplugged."""
    mode = settings.get("coop_mode", "off")
    if mode not in ("host", "join"):
        return {}
    server = "127.0.0.1" if mode == "host" else settings.get("coop_host", "").strip()
    invite = settings.get("coop_invite", "") if mode == "host" else settings.get("coop_join_invite", "").strip()
    return {
        "BB_SHADNET_SERVER": f"{server}:{GAME_PORT}",
        "BB_SHADNET_NPID": settings.get("coop_name") or "Hunter",
        "BB_SHADNET_PASSWORD": settings["coop_password"],
        "BB_SHADNET_INVITE": invite,
        "BB_SEAMLESS": "1",
    }


def problem(settings, server_binary):
    """Why co-op cannot start with these settings, or None."""
    mode = settings.get("coop_mode", "off")
    name = settings.get("coop_name", "")
    if mode == "off":
        return None
    if not name or len(name) > 16 or not name.replace("-", "").replace("_", "").isalnum():
        return "Player name: 1 to 16 letters, digits, - or _."
    if mode == "join" and not settings.get("coop_host", "").strip():
        return "Enter the host's address."
    if mode == "join" and not settings.get("coop_join_invite", "").strip():
        return "Enter the host's invite code."
    if mode == "host" and not Path(server_binary).is_file():
        return f"Server not built: {server_binary} (run scripts/build-server.sh)."
    return None


def write_config(directory, invite):
    """The server's shadnet.cfg (QSettings ini) and the data files it reads, in `directory`."""
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "shadnet.cfg").write_text(
        "[General]\n"
        "Host=0.0.0.0\n"
        f"UnsecuredPort={GAME_PORT}\n"
        f"MatchingUdpPort={STUN_PORT}\n"
        f"WebApiPort={WEB_PORT}\n"
        "Matching2Enabled=true\n"
        "BloodborneSeamlessCoop=true\n"
        f"RegistrationSecretKey={invite}\n"
        "EmailValidated=false\n"
        "StatsEnabled=false\n"
        "AdminsList=\n")


class HostServer:
    """The shadNet server process while hosting."""

    def __init__(self, binary, directory):
        self.binary = Path(binary)
        self.directory = Path(directory)
        self.process = None

    def start(self, invite, log, timeout=15):
        """Starts the server and waits until its WebAPI port answers. Raises RuntimeError."""
        write_config(self.directory, invite)
        for data in ("worlds.cfg", "scoreboards.cfg"):
            source = self.binary.parent / data
            if source.is_file():
                shutil.copy(source, self.directory / data)
        self.process = subprocess.Popen([str(self.binary)], cwd=self.directory, stdout=log,
                                        stderr=subprocess.STDOUT)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"the server exited ({self.process.returncode}); see its log")
            try:
                socket.create_connection(("127.0.0.1", WEB_PORT), timeout=0.5).close()
                return
            except OSError:
                time.sleep(0.2)
        self.stop()
        raise RuntimeError("the server did not start listening")

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.process = None
