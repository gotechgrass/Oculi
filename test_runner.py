"""Tests for `python -m oculi run --source NAME -- COMMAND ...` (standard library only).

The CLI tests start the real `python -m oculi` in a subprocess, so output and exit codes are
exactly what a shell would see. OCULI_STATE_FILE points at a temp folder, so the real
%LOCALAPPDATA%\\Oculi\\state.txt is never touched.

Run:  python -m unittest -v test_runner      (or: python test_runner.py)
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import oculi

HERE = os.path.dirname(os.path.abspath(oculi.__file__))
PY = sys.executable


class RunnerTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = tmp.name
        self.path = os.path.join(self.dir, "state.txt")
        env = mock.patch.dict(os.environ, {oculi.STATE_FILE_ENV: self.path})
        env.start()
        self.addCleanup(env.stop)

    def cli(self, *args, input=None):
        """Run `python -m oculi <args>` from the repo folder, like a user would."""
        return subprocess.run([PY, "-m", "oculi", *args], cwd=HERE, input=input,
                              capture_output=True, text=True, timeout=60)

    def run_cmd(self, source, *command, **kw):
        return self.cli("run", "--source", source, "--", *command, **kw)

    def state(self):
        event = oculi.read_state_file()
        return (event.state, event.source) if event else None

    # ---- events -------------------------------------------------------------
    def test_working_is_sent_before_the_command_runs(self):
        # The command itself reads the state file: it must already say "working".
        peek = "import os; print(open(os.environ['OCULI_STATE_FILE']).read().split()[0])"
        result = self.run_cmd("training", PY, "-c", peek)
        self.assertEqual(result.stdout, "working\n")
        self.assertEqual(self.state(), ("success", "training"))

    def test_exit_code_zero_produces_success(self):
        result = self.run_cmd("tests", PY, "-c", "pass")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.state(), ("success", "tests"))

    def test_nonzero_exit_code_produces_error(self):
        result = self.run_cmd("build", PY, "-c", "import sys; sys.exit(3)")
        self.assertEqual(result.returncode, 3)
        self.assertEqual(self.state(), ("error", "build"))

    def test_event_order_is_working_then_outcome(self):
        with mock.patch("oculi.send") as send:
            oculi.run_command("training", [PY, "-c", "pass"])
        self.assertEqual([c.args for c in send.call_args_list], [("training", "working"), ("training", "success")])
        with mock.patch("oculi.send") as send:
            oculi.run_command("training", [PY, "-c", "raise SystemExit(1)"])
        self.assertEqual([c.args for c in send.call_args_list], [("training", "working"), ("training", "error")])

    # ---- exit codes ---------------------------------------------------------
    def test_original_exit_code_is_returned(self):
        for code in (0, 1, 2, 42, 255):
            with self.subTest(code=code):
                result = self.run_cmd("x", PY, "-c", f"import sys; sys.exit({code})")
                self.assertEqual(result.returncode, code)

    @unittest.skipIf(os.name == "nt", "signals are POSIX-only")
    def test_command_killed_by_signal_returns_shell_style_code(self):
        result = self.run_cmd("x", PY, "-c", "import os, signal; os.kill(os.getpid(), signal.SIGTERM)")
        self.assertEqual(result.returncode, 128 + 15)
        self.assertEqual(self.state(), ("error", "x"))

    # ---- arguments ----------------------------------------------------------
    def test_command_arguments_are_preserved(self):
        args = ["a b", "--flag=1", "-x", "--", 'it\'s "quoted"', "", "ünï"]
        code = "import sys, json; print(json.dumps(sys.argv[1:]))"
        result = self.run_cmd("x", PY, "-c", code, *args)
        self.assertEqual(json.loads(result.stdout), args)  # spaces, quotes, empty string and a later '--' survive

    # ---- output is not swallowed ----------------------------------------------
    def test_stdout_and_stderr_pass_through_untouched(self):
        code = "import sys; sys.stdout.write('out line 1\\nout line 2'); sys.stderr.write('err line\\n')"
        result = self.run_cmd("x", PY, "-c", code)
        self.assertEqual(result.stdout, "out line 1\nout line 2")  # no extra text, no missing newline added
        self.assertEqual(result.stderr, "err line\n")

    def test_output_is_kept_when_the_command_fails(self):
        result = self.run_cmd("x", PY, "-c", "import sys; print('before'); sys.exit(9)")
        self.assertEqual((result.stdout, result.returncode), ("before\n", 9))

    def test_stdin_is_forwarded(self):
        result = self.run_cmd("x", PY, "-c", "import sys; sys.stdout.write(sys.stdin.read().upper())", input="hello\n")
        self.assertEqual(result.stdout, "HELLO\n")

    # ---- failure modes --------------------------------------------------------
    def test_command_not_found(self):
        result = self.run_cmd("x", "definitely-not-a-real-command-oculi")
        self.assertEqual(result.returncode, 127)
        self.assertIn("definitely-not-a-real-command-oculi", result.stderr)
        self.assertEqual(self.state(), ("error", "x"))  # the eye is not left on "working"

    def test_ctrl_c_marks_error_and_returns_130(self):
        with mock.patch("oculi.subprocess.call", side_effect=KeyboardInterrupt), \
                mock.patch("oculi.send") as send:
            self.assertEqual(oculi.run_command("x", [PY]), 130)
        self.assertEqual([c.args for c in send.call_args_list], [("x", "working"), ("x", "error")])

    def test_unexpected_exception_does_not_leave_working(self):
        with mock.patch("oculi.subprocess.call", side_effect=RuntimeError("boom")), \
                mock.patch("oculi.send") as send:
            with self.assertRaises(RuntimeError):
                oculi.run_command("x", [PY])
        self.assertEqual([c.args for c in send.call_args_list], [("x", "working"), ("x", "error")])

    def test_unwritable_state_file_does_not_stop_the_command(self):
        blocker = os.path.join(self.dir, "blocker")
        open(blocker, "w").close()                       # a file where the state folder would have to go
        with mock.patch.dict(os.environ, {oculi.STATE_FILE_ENV: os.path.join(blocker, "state.txt")}):
            result = self.run_cmd("x", PY, "-c", "import sys; print('ran'); sys.exit(7)")
        self.assertEqual((result.stdout, result.returncode, result.stderr), ("ran\n", 7, ""))

    # ---- argument handling of the CLI itself ----------------------------------
    def test_source_is_required_and_command_does_not_run(self):
        marker = os.path.join(self.dir, "ran")
        result = self.cli("run", "--", PY, "-c", f"open({marker!r}, 'w')")
        self.assertEqual(result.returncode, 2)
        self.assertFalse(os.path.exists(marker))
        self.assertFalse(os.path.exists(self.path))

    def test_empty_source_is_rejected_before_running(self):
        marker = os.path.join(self.dir, "ran")
        result = self.run_cmd("   ", PY, "-c", f"open({marker!r}, 'w')")
        self.assertEqual(result.returncode, 2)
        self.assertFalse(os.path.exists(marker))

    def test_command_is_required(self):
        self.assertEqual(self.cli("run", "--source", "x", "--").returncode, 2)
        self.assertEqual(self.cli("run", "--source", "x").returncode, 2)
        self.assertFalse(os.path.exists(self.path))

    def test_unknown_subcommand(self):
        self.assertEqual(self.cli("explode").returncode, 2)

    def test_no_arguments_still_runs_the_demo(self):
        result = self.cli()
        self.assertEqual(result.returncode, 0)
        self.assertIn("training", result.stdout)
        self.assertEqual(self.state(), ("error", "claude"))  # the demo's last event


if __name__ == "__main__":
    unittest.main(verbosity=2)
