"""Tests for oculi.py (standard library only).

Run:  python -m unittest -v test_oculi      (or: python test_oculi.py)
"""
import os
import time
import unittest
from unittest import mock

import oculi


class OculiTests(unittest.TestCase):
    def setUp(self):
        # An empty OCULI_STATE_FILE switches the bridge off, so these tests never touch the real state file.
        env = mock.patch.dict(os.environ, {oculi.STATE_FILE_ENV: ""})
        env.start()
        self.addCleanup(env.stop)
        self.o = oculi.Oculi()
        oculi.reset()

    # 1-5: every state is accepted
    def test_working_accepted(self):
        self._accepts("working")

    def test_success_accepted(self):
        self._accepts("success")

    def test_warning_accepted(self):
        self._accepts("warning")

    def test_error_accepted(self):
        self._accepts("error")

    def test_idle_accepted(self):
        self._accepts("idle")

    def _accepts(self, state):
        event = self.o.send(source="training", state=state)
        self.assertEqual(event.state, state)
        self.assertEqual(event.source, "training")
        self.assertEqual(self.o.current_state(), state)

    # 6: invalid states are rejected cleanly
    def test_invalid_states_rejected(self):
        for bad in ("fire", "", "  ", None, 42, ["working"]):
            with self.assertRaises(oculi.InvalidStateError, msg=repr(bad)):
                self.o.send(source="build", state=bad)
        self.assertEqual(self.o.events(), [])           # nothing was recorded
        self.assertEqual(self.o.current_state(), "idle")  # state unchanged

    def test_invalid_source_rejected(self):
        for bad in ("", "   ", None):
            with self.assertRaises(ValueError):
                self.o.send(source=bad, state="idle")

    # 7: several sources at once
    def test_multiple_sources(self):
        self.o.send("training", "working")
        self.o.send("tests", "warning")
        self.o.send("claude", "error")
        self.assertEqual(self.o.state_of("training"), "working")
        self.assertEqual(self.o.state_of("tests"), "warning")
        self.assertEqual(self.o.state_of("claude"), "error")
        self.assertIsNone(self.o.state_of("docker"))
        self.assertEqual(set(self.o.sources()), {"training", "tests", "claude"})
        self.assertEqual([e.source for e in self.o.events()], ["training", "tests", "claude"])
        self.assertEqual(self.o.current_state(), "error")  # latest event wins for now

    def test_source_updates_replace_its_previous_state(self):
        self.o.send("training", "working")
        self.o.send("training", "success")
        self.assertEqual(self.o.state_of("training"), "success")
        self.assertEqual(len(self.o.sources()), 1)
        self.assertEqual(len(self.o.events()), 2)

    # 8: timestamps are recorded
    def test_timestamps_recorded(self):
        before = time.time()
        first = self.o.send("build", "working")
        second = self.o.send("build", "success")
        after = time.time()
        self.assertIsInstance(first.timestamp, float)
        self.assertTrue(before <= first.timestamp <= second.timestamp <= after)

    def test_timestamp_uses_injected_clock(self):
        ticks = iter([100.0, 200.5])
        o = oculi.Oculi(clock=lambda: next(ticks))
        self.assertEqual(o.send("a", "idle").timestamp, 100.0)
        self.assertEqual(o.send("b", "idle").timestamp, 200.5)

    # extras
    def test_default_state_is_idle(self):
        self.assertEqual(self.o.current_state(), "idle")
        self.assertIsNone(self.o.current_event())

    def test_visual_mapping_covers_every_state(self):
        self.assertEqual(set(oculi.VISUALS), set(oculi.STATES))
        expected = {
            "idle": ("static", None),
            "working": ("rotate", None),
            "success": ("glow_blink", "green"),
            "warning": ("pulse", "amber"),
            "error": ("glow_pulse", "red"),
        }
        for state, (behavior, color) in expected.items():
            v = oculi.VISUALS[state]
            self.assertEqual((v.behavior, v.color), (behavior, color))

    def test_visual_follows_current_state(self):
        self.o.send("tests", "error")
        self.assertEqual(self.o.visual(), oculi.VISUALS["error"])

    def test_event_to_dict(self):
        d = self.o.send("vscode", "idle").to_dict()
        self.assertEqual(set(d), {"source", "state", "timestamp"})

    def test_module_level_api(self):
        oculi.send(source="training", state="working")
        oculi.send(source="claude", state="success")
        self.assertEqual(oculi.current_state(), "success")
        self.assertEqual(oculi.state_of("training"), "working")
        with self.assertRaises(oculi.InvalidStateError):
            oculi.send(source="claude", state="nope")


if __name__ == "__main__":
    unittest.main(verbosity=2)
