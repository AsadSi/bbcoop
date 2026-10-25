# SPDX-License-Identifier: GPL-2.0-or-later
"""bbcoop: Host and Join for the launchers (GTK on Linux, Tk on macOS and Windows), without a toolkit.

Host: while the launcher is open this PC runs the group's server: shadNet (server/: accounts, bells
and summons) and scripts/bbsc_gameserver (ss.info, the UDP 3478 relay, messages). Friends reach it
over Tailscale at this PC's Tailscale address. Join: a host found on the tailnet, or typed in.
Before the game starts the launcher checks that the host runs the same build (the game server's
hello) and makes this install's account there (shadNet's Create command; its WebAPI has none).
Asad's decisions, 2026-10-24: docs/bbcoop/brief.md, "Easy online".
"""

from concurrent.futures import ThreadPoolExecutor
import ipaddress
import json
import os
import secrets
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path
import urllib.request

GAME_PORT, STUN_PORT, WEB_PORT, SS_PORT = 31313, 31314, 31315, 20443
HELLO = "/bbcoop/hello"
TAILSCALE_APP = "/Applications/Tailscale.app/Contents/MacOS/Tailscale"
# shadNet's errors (server/src/protocol.h) that the account step explains.
INVALID_INPUT, ALREADY_LOGGED_IN, INVALID_PASSWORD, EXISTING_USERNAME, UNAUTHORIZED = 3, 6, 8, 11, 23


def build_id(port_dir):
    """This build: the BUILD file a package carries, else the checkout's commit, with '+' when
    tracked files differ from it. '' when neither is known."""
    stamp = Path(port_dir) / "BUILD"
    if stamp.is_file():
        return stamp.read_text(encoding="utf-8").strip()
    try:
        git = ["git", "-C", str(port_dir)]
        commit = subprocess.run(git + ["rev-parse", "--short=12", "HEAD"], capture_output=True,
                                text=True, timeout=5, check=True).stdout.strip()
        changed = subprocess.run(git + ["status", "--porcelain", "--untracked-files=no",
                                        "--ignore-submodules"], capture_output=True, text=True,
                                 timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""
    return commit + ("+" if changed else "")


def build_problem(ours, theirs, host):
    """Why this build cannot join the host's, or None. Local changes ('+') are not compared."""
    if not theirs or not ours:
        return None
    if ours.rstrip("+") != theirs.rstrip("+"):
        return (f"{host} runs build {theirs}, this is {ours}. Co-op needs the same build on both "
                "sides: install the host's download. If you both already have it, the host picks "
                "Host again in their launcher.")
    return None


def valid_name(name):
    """shadNet's rule for an account name (server/src/client_session.h, IsValidNpid)."""
    return 3 <= len(name) <= 16 and name.replace("-", "").replace("_", "").isalnum() \
        and name.isascii() and name != "DeletedUser"


def new_password():
    return secrets.token_urlsafe(18)


def tailscale_cli():
    return next((cli for cli in (shutil.which("tailscale"), TAILSCALE_APP) if cli and Path(cli).exists()), None)


def tailscale_status():
    """`tailscale status --json`, or None without Tailscale (or while its service is not up)."""
    cli = tailscale_cli()
    if not cli:
        return None
    try:
        run = subprocess.run([cli, "status", "--json"], capture_output=True, text=True, timeout=5)
        return json.loads(run.stdout[run.stdout.find("{"):])  # a warning line may come first
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


def tailscale_address():
    """This computer's Tailscale IPv4 from the routing table (the route to Tailscale's own
    100.100.100.100 leaves from it), or '': no Tailscale command needed (Taha's Mac could not read
    its status from the app, 2026-10-25)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(("100.100.100.100", 53))  # no packet is sent
            address = probe.getsockname()[0]
    except OSError:
        return ""
    return address if ipaddress.ip_address(address) in ipaddress.ip_network("100.64.0.0/10") else ""


def connect_tailscale(timeout=20):
    """Host and Join connect Tailscale themselves (Asad, 2026-10-25): its Mac app is started in the
    background, then `tailscale up`. '' when connected, else what the player has to do."""
    cli = tailscale_cli()
    if tailscale_address():
        return ""
    if not cli:
        return "Tailscale is not installed: get it at tailscale.com/download and sign in."
    if (tailscale_status() or {}).get("BackendState") == "Running":
        return ""
    if sys.platform == "darwin":
        subprocess.run(["open", "-g", "-a", "Tailscale"], capture_output=True)
    asked, read = False, False
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = tailscale_status()
        read = read or status is not None
        state = (status or {}).get("BackendState")
        if state == "Running" or (not read and time.monotonic() > deadline - timeout + 5):
            # Its state unreadable from here (Taha's Mac, 2026-10-25, while connected): don't
            # block; the host check that follows tells whether the host is reachable.
            return ""
        if state == "NeedsLogin":
            return "Sign in to Tailscale (its menu bar icon), then pick Host or Join again."
        if state == "Stopped" and not asked:  # turned off in Tailscale: on again
            asked = True
            try:
                up = subprocess.run([cli, "up", "--timeout=10s"], capture_output=True, text=True, timeout=15)
                # Refused when settings differ from the defaults (accept routes, ...): Tailscale
                # prints the command that states them all, so nothing changes but on/off.
                again = next((line.split()[1:] for line in up.stderr.splitlines()
                              if line.strip().startswith("tailscale up ")), None)
                if up.returncode and again:
                    subprocess.run([cli, *again], capture_output=True, timeout=15)
            except (OSError, subprocess.SubprocessError):
                pass
        time.sleep(0.5)
    return "Tailscale did not connect: connect it (its menu bar icon), then pick Host or Join again."


def ipv4(addresses):
    return next((address for address in addresses or () if "." in address), "")


def my_address():
    """(address friends use, note): the Tailscale IPv4, else this PC's LAN address with a note."""
    status = tailscale_status()
    if status and status.get("BackendState") == "Running":
        address = ipv4(status.get("Self", {}).get("TailscaleIPs"))
        if address:
            return address, ""
    if tailscale_address():
        return tailscale_address(), ""
    note = "Tailscale is not connected: only players on your own network can join."
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(("10.255.255.255", 1))  # no packet is sent; picks the outgoing interface
            return probe.getsockname()[0], note
    except OSError:
        return "127.0.0.1", note


def hello(address, timeout=3):
    """The host's hello from its game server ({'name', 'build'}), or None."""
    try:
        with urllib.request.urlopen(f"http://{address}:{SS_PORT}{HELLO}", timeout=timeout) as reply:
            answer = json.loads(reply.read(4096))
        return answer if isinstance(answer, dict) else None
    except (OSError, ValueError):
        return None


def find_hosts(timeout=1.5):
    """Online tailnet peers hosting (TCP 31313 answers): [(address, name, build)], by name, or None
    when Tailscale is not connected. A host without a hello (a server started by hand) is listed by
    its machine name."""
    status = tailscale_status()
    if not status or status.get("BackendState") != "Running":
        return [] if tailscale_address() else None  # connected, but its peers unreadable: type one
    peers = [(ipv4(peer.get("TailscaleIPs")), peer.get("HostName") or "")
             for peer in (status.get("Peer") or {}).values() if peer.get("Online")]

    def probe(peer):
        address, machine = peer
        try:
            socket.create_connection((address, GAME_PORT), timeout=timeout).close()
        except OSError:
            return None
        answer = hello(address, timeout) or {}
        return address, str(answer.get("name") or machine), str(answer.get("build") or "")

    peers = [peer for peer in peers if peer[0]]
    if not peers:
        return []
    with ThreadPoolExecutor(max_workers=min(16, len(peers))) as pool:
        found = [host for host in pool.map(probe, peers) if host]
    return sorted(found, key=lambda host: host[1].lower())


def join_without_host(settings):
    """Join picked but no host: the game starts offline (Asad, 2026-10-25), as a first start does."""
    return settings.get("coop_mode") == "join" and not str(settings.get("coop_join") or "").strip()


def server_fields(address):
    """The launcher's three server settings for a host address."""
    return {"online_server": f"{address}:{GAME_PORT}", "online_webapi": f"http://{address}:{WEB_PORT}",
            "online_community": f"http://{address}"}


# ---- accounts: shadNet's own protocol (server/src/protocol.h, client_session.cpp) -------------

def _varint(value):
    out = bytearray()
    while True:
        out.append((value & 0x7F) | (0x80 if value > 0x7F else 0))
        value >>= 7
        if not value:
            return bytes(out)


def _strings(*values):
    """A protobuf message of string fields 1, 2, ... (RegistrationRequest, LoginRequest)."""
    out = b""
    for number, value in enumerate(values, 1):
        data = value.encode()
        out += _varint(number << 3 | 2) + _varint(len(data)) + data
    return out


def _receive(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise OSError("the server closed the connection")
        data += chunk
    return data


def _packet(sock):
    kind, command, size, packet = struct.unpack("<BHIQ", _receive(sock, 15))
    return kind, command, packet, _receive(sock, size - 15)


def _call(sock, command, message, packet):
    """One request; the reply's error byte."""
    body = struct.pack("<I", len(message)) + message
    sock.sendall(struct.pack("<BHIQ", 0, command, 15 + len(body), packet) + body)
    while True:
        kind, _command, reply_to, payload = _packet(sock)
        if kind == 1 and reply_to == packet:
            return payload[0] if payload else 0


def make_account(address, name, password, timeout=8):
    """Makes this install's account on the host's shadNet server: 'created', or 'exists' when it
    is there already with this password. Raises ValueError with the reason otherwise."""
    try:
        with socket.create_connection((address, GAME_PORT), timeout=timeout) as sock:
            sock.settimeout(timeout)
            _packet(sock)  # ServerInfo
            # Create (2): npid, password, avatar (the server's default), email (required, unused).
            error = _call(sock, 2, _strings(name, password, "", f"{name.lower()}@bbcoop.invalid"), 1)
            if error == 0:
                return "created"
            if error == EXISTING_USERNAME:
                error = _call(sock, 0, _strings(name, password), 2)  # Login: is it ours?
                if error in (0, ALREADY_LOGGED_IN):
                    return "exists"
                if error == INVALID_PASSWORD:
                    raise ValueError(f'The hunter name "{name}" is taken on this host. '
                                     "Pick another name.")
    except OSError as failure:
        raise ValueError(f"The host's server does not answer ({failure}).") from failure
    if error == INVALID_INPUT:
        raise ValueError("Hunter name: 3 to 16 letters, digits, - or _.")
    if error == UNAUTHORIZED:
        raise ValueError("This server takes new accounts only with its owner's key.")
    raise ValueError(f"The host's server refused the account (shadNet error {error}).")


def prepare_join(address, name, password, build):
    """Checks the host and makes the account before the game starts; raises ValueError."""
    answer = hello(address)
    if answer is None:
        try:
            socket.create_connection((address, GAME_PORT), timeout=4).close()
        except OSError:
            raise ValueError(f"Nobody is hosting at {address}: the host picks Host in their "
                             "launcher, and both of you need Tailscale on.") from None
        raise ValueError(f"{address} runs a server but no bbcoop host (its game server on "
                         f"port {SS_PORT} does not answer).")
    problem = build_problem(build, str(answer.get("build") or ""), answer.get("name") or address)
    if problem:
        raise ValueError(problem)
    return make_account(address, name, password)


# ---- hosting ---------------------------------------------------------------------------------

def shadnet_binary(port_dir):
    """The shadNet server: a package's (bin/), else a source build (scripts/build-server.sh)."""
    port_dir = Path(port_dir)
    for candidate in (port_dir / "bin" / "shadnet.app" / "Contents" / "MacOS" / "shadnet",
                      port_dir / "bin" / "shadnet", port_dir / "server" / "build-mac" / "shadnet",
                      port_dir / "server" / "build-linux" / "shadnet"):
        if candidate.is_file():
            return candidate
    return None


def write_config(directory, address):
    """shadNet's shadnet.cfg (QSettings ini): Bloodborne's co-op on, open registration (only the
    tailnet reaches this address), no stats server."""
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "shadnet.cfg").write_text(
        "[General]\n"
        f"Host={address}\n"
        f"UnsecuredPort={GAME_PORT}\n"
        f"MatchingUdpPort={STUN_PORT}\n"
        f"WebApiPort={WEB_PORT}\n"
        "Matching2Enabled=true\n"
        "BloodborneSeamlessCoop=true\n"
        "RegistrationSecretKey=\n"
        "EmailValidated=false\n"
        "StatsEnabled=false\n"
        "AdminsList=\n", encoding="utf-8")


def _wait(check, process, what, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"{what} exited ({process.returncode}); see its log")
        if check():
            return
        time.sleep(0.2)
    raise RuntimeError(f"{what} did not start")


def _answers(address, port):
    try:
        socket.create_connection((address, port), timeout=0.5).close()
        return True
    except OSError:
        return False


class Host:
    """The group's server on this PC while hosting. `python` runs a script: [python] or, frozen,
    [the launcher, '--script']."""

    def __init__(self, port_dir, data_dir, python):
        self.port_dir, self.directory = Path(port_dir).resolve(), Path(data_dir).resolve() / "host"
        self.python = python
        self.processes = []
        self.address = ""
        self.build = ""  # what its hello answers

    def running(self):
        return bool(self.processes) and all(process.poll() is None for process in self.processes)

    def start(self, address, name, build):
        """Starts shadNet and the game server on `address`; raises RuntimeError."""
        self.stop_leftovers()
        self.stop()
        for port in (GAME_PORT, SS_PORT):
            if _answers(address, port):
                raise RuntimeError(f"Port {port} is in use: another server runs on this computer.")
        binary = shadnet_binary(self.port_dir)
        if binary is None:
            raise RuntimeError("This build has no server (scripts/build-server.sh builds it).")
        server = self.directory / "shadnet"
        write_config(server, address)
        for data in ("worlds.cfg", "scoreboards.cfg"):
            for source in (binary.parent / data, self.port_dir / "server" / data):
                if source.is_file():
                    shutil.copy(source, server / data)
                    break
        (server / "db").mkdir(exist_ok=True)
        logs = self.directory / "logs"
        logs.mkdir(parents=True, exist_ok=True)
        flags = {"start_new_session": True} if sys.platform != "win32" else \
            {"creationflags": getattr(subprocess, "CREATE_NO_WINDOW", 0)}
        try:
            with open(logs / "shadnet.log", "ab") as log:
                shadnet = subprocess.Popen([str(binary)], cwd=server, stdout=log,
                                           env={**os.environ, "SHADNET_HOME": str(server)},
                                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **flags)
            self.processes.append(shadnet)
            _wait(lambda: _answers(address, GAME_PORT), shadnet, "The shadNet server", 15)
            script = self.port_dir / "scripts" / "bbsc_gameserver" / "bbsc_gameserver.py"
            with open(logs / "gameserver-console.log", "ab") as log:
                game = subprocess.Popen(
                    [*self.python, str(script), "--shadnet", address, "--host", address,
                     "--advertise", address, "--data", str(self.directory / "online-data.json"),
                     "--log-dir", str(logs), "--bbcoop-name", name, "--bbcoop-build", build],
                    cwd=script.parent, stdout=log, stderr=subprocess.STDOUT,
                    stdin=subprocess.DEVNULL, **flags)
            self.processes.append(game)
            (self.directory / "pids").write_text(" ".join(str(p.pid) for p in self.processes))
            _wait(lambda: hello(address, 0.5) is not None, game, "The game server", 15)
        except (OSError, RuntimeError) as failure:
            self.stop()
            raise RuntimeError(str(failure)) from failure
        self.address, self.build = address, build

    def stop(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
        self.processes = []
        self.address, self.build = "", ""
        (self.directory / "pids").unlink(missing_ok=True)

    def stop_leftovers(self):
        """The servers of a launcher that was killed instead of closed: they would hold the ports."""
        try:
            pids = [int(pid) for pid in (self.directory / "pids").read_text().split()]
        except (OSError, ValueError):
            return
        for pid in pids:
            try:  # only ours: a reused pid runs something else
                command = subprocess.run(["ps", "-p", str(pid), "-o", "command="], capture_output=True,
                                         text=True, timeout=5).stdout
                if "shadnet" in command or "bbsc_gameserver" in command:
                    os.kill(pid, signal.SIGTERM)
            except (OSError, subprocess.SubprocessError):
                pass
        time.sleep(0.5)
