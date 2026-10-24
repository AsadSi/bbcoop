"""bbcoop: the launchers' Host and Join (launcher/bbcoop_online.py, no GTK or Tk)."""
from paths import ROOT
import socket
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'launcher'))
import bbcoop_online  # noqa: E402


class RulesTests(unittest.TestCase):
    def test_build_problem(self):
        self.assertIsNone(bbcoop_online.build_problem('abc', 'abc', 'Eileen'))
        self.assertIsNone(bbcoop_online.build_problem('abc+', 'abc', 'Eileen'))  # local changes
        self.assertIsNone(bbcoop_online.build_problem('abc', '', 'Eileen'))  # an older host
        problem = bbcoop_online.build_problem('abc', 'def', 'Eileen')
        self.assertIn('Eileen runs build def, this is abc', problem)

    def test_build_id(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / 'BUILD').write_text('0123456789ab\n')
            self.assertEqual(bbcoop_online.build_id(directory), '0123456789ab')
        self.assertRegex(bbcoop_online.build_id(ROOT), r'^[0-9a-f]{12}\+?$')

    def test_names(self):
        for name in ('Eileen', 'old_hunter-2', 'abc'):
            self.assertTrue(bbcoop_online.valid_name(name), name)
        for name in ('ab', 'a' * 17, 'a b', 'Gehrmanø', 'DeletedUser', ''):
            self.assertFalse(bbcoop_online.valid_name(name), name)

    def test_server_fields(self):
        self.assertEqual(bbcoop_online.server_fields('100.64.0.7'), {
            'online_server': '100.64.0.7:31313', 'online_webapi': 'http://100.64.0.7:31315',
            'online_community': 'http://100.64.0.7'})

    def test_registration_message(self):
        # protobuf: field 1 "abc" (0x0a 3 ...), field 2 "" (0x12 0)
        self.assertEqual(bbcoop_online._strings('abc', ''), b'\x0a\x03abc\x12\x00')
        self.assertEqual(bbcoop_online._varint(300), b'\xac\x02')

    def test_server_config(self):
        with tempfile.TemporaryDirectory() as directory:
            bbcoop_online.write_config(Path(directory), '100.64.0.7')
            config = (Path(directory) / 'shadnet.cfg').read_text().splitlines()
            for line in ('Host=100.64.0.7', 'RegistrationSecretKey=', 'BloodborneSeamlessCoop=true',
                         'Matching2Enabled=true', 'WebApiPort=31315'):
                self.assertIn(line, config)


@unittest.skipUnless(bbcoop_online.shadnet_binary(ROOT), 'no shadNet build (scripts/build-server.sh)')
class HostTests(unittest.TestCase):
    """The real server and game server on 127.0.0.1: hello, accounts, the build check."""

    def setUp(self):
        for port in (bbcoop_online.GAME_PORT, bbcoop_online.SS_PORT):
            with socket.socket() as probe:
                if probe.connect_ex(('127.0.0.1', port)) == 0:
                    self.skipTest(f'port {port} is in use')
        self.directory = tempfile.TemporaryDirectory()
        self.host = bbcoop_online.Host(ROOT, self.directory.name, [sys.executable])
        self.host.start('127.0.0.1', 'Eileen', 'abc')
        self.addCleanup(self.directory.cleanup)
        self.addCleanup(self.host.stop)

    def test_host(self):
        self.assertTrue(self.host.running())
        self.assertEqual(bbcoop_online.hello('127.0.0.1'), {'name': 'Eileen', 'build': 'abc'})
        self.assertEqual(bbcoop_online.prepare_join('127.0.0.1', 'Djura', 'pw1', 'abc'), 'created')
        self.assertEqual(bbcoop_online.prepare_join('127.0.0.1', 'Djura', 'pw1', 'abc+'), 'exists')
        with self.assertRaisesRegex(ValueError, 'taken'):
            bbcoop_online.make_account('127.0.0.1', 'Djura', 'pw2')
        with self.assertRaisesRegex(ValueError, 'runs build abc, this is def'):
            bbcoop_online.prepare_join('127.0.0.1', 'Alfred', 'pw', 'def')
        processes = list(self.host.processes)
        self.host.stop()
        self.assertTrue(all(process.poll() is not None for process in processes))
        with self.assertRaisesRegex(ValueError, 'Nobody is hosting'):
            bbcoop_online.prepare_join('127.0.0.1', 'Alfred', 'pw', 'abc')


if __name__ == '__main__':
    unittest.main()
