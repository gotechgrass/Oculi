"""Tests for the Python -> C++ state-file bridge (standard library only).

Every test points OCULI_STATE_FILE at a temporary folder, so the real
%LOCALAPPDATA%\\Oculi\\state.txt is never touched.

Run:  python -m unittest -v test_bridge      (or: python test_bridge.py)
"""
import os
import tempfile
import unittest
from unittest import mock

import oculi


class BridgeTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = tmp.name
        self.path = os.path.join(self.dir, "state.txt")
        env = mock.patch.dict(os.environ, {oculi.STATE_FILE_ENV: self.path})
        env.start()
        self.addCleanup(env.stop)
        self.o = oculi.Oculi()

    def read_raw(self):
        with open(self.path, encoding="utf-8", newline="") as f:
            return f.read()

    # ---- file content ----------------------------------------------------
    def test_send_writes_state_timestamp_source(self):
        event = self.o.send("training", "working")
        raw = self.read_raw()
        self.assertTrue(raw.endswith("\n"))
        self.assertEqual(raw.count("\n"), 1)
        state, timestamp, source = raw.split()
        self.assertEqual(state, "working")
        self.assertAlmostEqual(float(timestamp), event.timestamp, places=5)
        self.assertEqual(source, "training")

    def test_every_state_is_written(self):
        for state in oculi.STATES:
            self.o.send("tests", state)
            self.assertEqual(self.read_raw().split()[0], state)

    def test_read_state_file_round_trip(self):
        sent = self.o.send("build", "warning")
        got = oculi.read_state_file()
        self.assertEqual((got.state, got.source), ("warning", "build"))
        self.assertAlmostEqual(got.timestamp, sent.timestamp, places=5)

    def test_multiple_sources_latest_event_is_in_file(self):
        self.o.send("training", "working")
        self.o.send("tests", "warning")
        self.o.send("claude", "error")
        self.assertEqual(self.read_raw().split()[0:3:2], ["error", "claude"])
        self.assertEqual(self.o.state_of("training"), "working")  # per-source memory still works

    def test_source_with_spaces_stays_one_token(self):
        self.o.send("my build", "idle")
        self.assertEqual(len(self.read_raw().split()), 3)
        self.assertEqual(oculi.read_state_file().source, "my_build")

    def test_module_level_send_updates_file(self):
        oculi.reset()
        oculi.send(source="docker", state="success")
        self.assertEqual(self.read_raw().split()[0], "success")
        self.assertEqual(self.read_raw().split()[2], "docker")

    def test_reset_publishes_idle(self):
        self.o.send("claude", "error")
        self.o.reset()
        self.assertEqual(oculi.read_state_file().state, "idle")

    # ---- rejected input --------------------------------------------------
    def test_invalid_state_leaves_file_untouched(self):
        with self.assertRaises(oculi.InvalidStateError):
            self.o.send("build", "fire")
        self.assertFalse(os.path.exists(self.path))      # nothing written yet
        self.o.send("build", "working")
        before = self.read_raw()
        with self.assertRaises(oculi.InvalidStateError):
            self.o.send("build", "fire")
        self.assertEqual(self.read_raw(), before)        # still the old, valid line

    # ---- atomic writes ---------------------------------------------------
    def test_write_is_temp_file_plus_replace(self):
        with mock.patch("oculi.os.replace", wraps=os.replace) as replace:
            self.o.send("build", "working")
        self.assertEqual(replace.call_count, 1)
        tmp, dest = replace.call_args[0]
        self.assertEqual(dest, self.path)
        self.assertNotEqual(tmp, dest)
        self.assertEqual(os.listdir(self.dir), ["state.txt"])  # no temp file left behind

    def test_failed_replace_never_raises_and_cleans_up(self):
        with mock.patch("oculi.os.replace", side_effect=PermissionError), mock.patch("oculi.time.sleep"):
            event = self.o.send("build", "error")             # must not raise
        self.assertEqual(event.state, "error")
        self.assertEqual(self.o.current_state(), "error")     # in-memory state is unaffected
        self.assertEqual(os.listdir(self.dir), [])            # no temp file left behind

    def test_missing_folder_is_created(self):
        nested = os.path.join(self.dir, "a", "b", "state.txt")
        with mock.patch.dict(os.environ, {oculi.STATE_FILE_ENV: nested}):
            self.o.send("build", "idle")
        self.assertTrue(os.path.exists(nested))

    # ---- path handling ---------------------------------------------------
    def test_env_override_sets_path(self):
        self.assertEqual(oculi.state_file_path(), self.path)

    def test_empty_env_disables_bridge(self):
        with mock.patch.dict(os.environ, {oculi.STATE_FILE_ENV: ""}):
            self.assertIsNone(oculi.state_file_path())
            self.o.send("build", "working")                   # still works, just no file
        self.assertFalse(os.path.exists(self.path))

    def test_default_path_uses_localappdata(self):
        env = {k: v for k, v in os.environ.items() if k != oculi.STATE_FILE_ENV}
        env["LOCALAPPDATA"] = os.path.join("C:", "Users", "x", "AppData", "Local")
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertEqual(oculi.state_file_path(), os.path.join(env["LOCALAPPDATA"], "Oculi", "state.txt"))

    def test_bridge_can_be_turned_off_per_instance(self):
        quiet = oculi.Oculi(bridge=False)
        quiet.send("build", "working")
        self.assertFalse(os.path.exists(self.path))

    def test_read_state_file_handles_missing_and_garbage(self):
        self.assertIsNone(oculi.read_state_file())            # missing
        for bad in ("", "nonsense\n", "working abc src\n", "fire 1.0 src\n"):
            with open(self.path, "w") as f:
                f.write(bad)
            self.assertIsNone(oculi.read_state_file(), msg=repr(bad))

    # ---- state lifetime (decided behaviour) ----------------------------
    def test_success_returns_to_idle_after_about_three_seconds(self):
        now = [1000.0]
        o = oculi.Oculi(clock=lambda: now[0])
        o.send("training", "success")
        self.assertEqual(o.current_state(), "success")
        now[0] = 1002.9
        self.assertEqual(o.current_state(), "success")
        now[0] = 1003.0
        self.assertEqual(o.current_state(), "idle")
        self.assertEqual(oculi.read_state_file().state, "success")  # file keeps the event; C++ applies the same 3 s rule

    def test_other_states_persist_until_next_event(self):
        now = [1000.0]
        o = oculi.Oculi(clock=lambda: now[0])
        for state in ("idle", "working", "warning", "error"):
            o.send("training", state)
            now[0] += 3600                                    # an hour later
            self.assertEqual(o.current_state(), state)

    def test_new_event_replaces_success_immediately(self):
        now = [1000.0]
        o = oculi.Oculi(clock=lambda: now[0])
        o.send("training", "success")
        now[0] += 1
        o.send("build", "working")
        self.assertEqual(o.current_state(), "working")
        self.assertEqual(oculi.read_state_file().state, "working")


if __name__ == "__main__":
    unittest.main(verbosity=2)
