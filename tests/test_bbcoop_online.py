"""bbcoop: the launcher's co-op settings, environment and host server lifecycle (no GTK)."""
from paths import ROOT
import socket
import stat
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'launcher'))
import bbcoop_online  # noqa: E402


class SettingsTests(unittest.TestCase):
    def test_secrets_fill_once(self):
        settings = {}
        self.assertTrue(bbcoop_online.ensure_secrets(settings))
        invite, password = settings['coop_invite'], settings['coop_password']
        self.assertRegex(invite, r'^[a-z2-9]{4}-[a-z2-9]{4}-[a-z2-9]{4}$')
        self.assertNotEqual(invite, password)
        self.assertFalse(bbcoop_online.ensure_secrets(settings))
        self.assertEqual(settings['coop_invite'], invite)

    def test_environment(self):
        base = {'coop_name': 'Eileen', 'coop_password': 'pw', 'coop_invite': 'mine',
                'coop_host': ' 100.64.0.7 ', 'coop_join_invite': ' theirs '}
        self.assertEqual(bbcoop_online.environment({**base, 'coop_mode': 'off'}), {})
        host = bbcoop_online.environment({**base, 'coop_mode': 'host'})
        self.assertEqual(host['BB_SHADNET_SERVER'], '127.0.0.1:31313')
        self.assertEqual(host['BB_SHADNET_INVITE'], 'mine')
        self.assertEqual(host['BB_SEAMLESS'], '1')
        join = bbcoop_online.environment({**base, 'coop_mode': 'join'})
        self.assertEqual(join['BB_SHADNET_SERVER'], '100.64.0.7:31313')
        self.assertEqual(join['BB_SHADNET_INVITE'], 'theirs')
        self.assertEqual(join['BB_SHADNET_NPID'], 'Eileen')

    def test_problems(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'shadnet'
            ok = {'coop_mode': 'join', 'coop_name': 'Eileen', 'coop_host': '100.64.0.7',
                  'coop_join_invite': 'abcd'}
            self.assertIsNone(bbcoop_online.problem({'coop_mode': 'off'}, binary))
            self.assertIsNone(bbcoop_online.problem(ok, binary))
            self.assertIn('name', bbcoop_online.problem({**ok, 'coop_name': ''}, binary))
            self.assertIn('name', bbcoop_online.problem({**ok, 'coop_name': 'a b'}, binary))
            self.assertIn('address', bbcoop_online.problem({**ok, 'coop_host': ' '}, binary))
            self.assertIn('invite', bbcoop_online.problem({**ok, 'coop_join_invite': ''}, binary))
            self.assertIn('not built', bbcoop_online.problem({**ok, 'coop_mode': 'host'}, binary))
            binary.touch()
            self.assertIsNone(bbcoop_online.problem({**ok, 'coop_mode': 'host'}, binary))

    def test_server_config(self):
        with tempfile.TemporaryDirectory() as directory:
            bbcoop_online.write_config(Path(directory), 'abcd-efgh-jkmn')
            config = (Path(directory) / 'shadnet.cfg').read_text()
            for line in ('Host=0.0.0.0', 'RegistrationSecretKey=abcd-efgh-jkmn', 'BloodborneSeamlessCoop=true',
                         'Matching2Enabled=true', 'WebApiPort=31315'):
                self.assertIn(line, config.splitlines())


class HostServerTests(unittest.TestCase):
    """HostServer against a stand-in that listens on the WebAPI port like the real server."""

    def setUp(self):
        probe = socket.socket()
        if probe.connect_ex(('127.0.0.1', bbcoop_online.WEB_PORT)) == 0:
            probe.close()
            self.skipTest('port 31315 is in use')
        probe.close()

    def stand_in(self, directory, body):
        binary = Path(directory) / 'bin' / 'shadnet'
        binary.parent.mkdir()
        binary.write_text(f'#!{sys.executable}\n{body}')
        binary.chmod(binary.stat().st_mode | stat.S_IXUSR)
        (binary.parent / 'worlds.cfg').write_text('worlds')
        return binary

    def test_start_and_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = self.stand_in(directory, (
                'import socket, time\n'
                's = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n'
                f's.bind(("127.0.0.1", {bbcoop_online.WEB_PORT})); s.listen()\n'
                'while True: time.sleep(1)\n'))
            server = bbcoop_online.HostServer(binary, Path(directory) / 'run')
            with open(Path(directory) / 'log', 'w') as log:
                server.start('abcd', log)
            self.assertIn('RegistrationSecretKey=abcd', (Path(directory) / 'run' / 'shadnet.cfg').read_text())
            self.assertEqual((Path(directory) / 'run' / 'worlds.cfg').read_text(), 'worlds')
            process = server.process
            server.stop()
            self.assertIsNotNone(process.poll())
            self.assertIsNone(server.process)

    def test_server_that_exits(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = self.stand_in(directory, 'raise SystemExit(3)\n')
            server = bbcoop_online.HostServer(binary, Path(directory) / 'run')
            with open(Path(directory) / 'log', 'w') as log, self.assertRaisesRegex(RuntimeError, 'exited'):
                server.start('abcd', log, timeout=5)


if __name__ == '__main__':
    unittest.main()
