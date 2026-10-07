"""oculi.py - the event/state layer for Oculi.

Python only tracks *what Oculi should be feeling*; it does not draw anything.
Any program can report its status with:

    import oculi
    oculi.send(source="training", state="working")
    oculi.send(source="training", state="success")

Standard library only. Every `send()` also updates a small state file that the
C++ widget polls (the bridge):

    %LOCALAPPDATA%\\Oculi\\state.txt      one line:  <state> <timestamp> <source>

Set the OCULI_STATE_FILE environment variable to use another path (an empty
value turns the bridge off). `success` lasts SUCCESS_SECONDS, then counts as idle;
every other state stays until the next event.

Watch any command without changing it (no `import oculi` needed in the command):

    python -m oculi run --source training -- python train.py
    python -m oculi run --source tests -- pytest -q

working is sent first, then success (exit code 0) or error (anything else). The
command's output is not touched and its exit code becomes ours.
"""
import argparse
import os
import shutil
import subprocess
import sys
import threading
import time
from collections import deque
from dataclasses import asdict, dataclass
from typing import Callable, Deque, Dict, List, Optional

STATES = ("idle", "working", "success", "warning", "error")
DEFAULT_STATE = "idle"
SUCCESS_SECONDS = 3.0           # `success` is shown this long, then the eye returns to idle
STATE_FILE_ENV = "OCULI_STATE_FILE"

# Examples only. Any non-empty string is accepted as a source.
KNOWN_SOURCES = ("claude", "vscode", "training", "build", "tests", "docker", "other")


@dataclass(frozen=True)
class Visual:
    """Intended look for a logical state (the renderer decides how to draw it)."""
    behavior: str
    color: Optional[str]
    description: str


VISUALS: Dict[str, Visual] = {
    "idle":    Visual("static",     None,    "Normal, static eye"),
    "working": Visual("rotate",     None,    "Loading / rotating animation"),
    "success": Visual("glow_blink", "green", "Green glow + short blink"),
    "warning": Visual("pulse",      "amber", "Yellow/amber pulse"),
    "error":   Visual("glow_pulse", "red",   "Red glow / pulse"),
}


class InvalidStateError(ValueError):
    """Raised when an event uses a state that Oculi does not know."""


@dataclass(frozen=True)
class Event:
    source: str
    state: str
    timestamp: float  # seconds since the epoch (time.time())

    def to_dict(self) -> dict:
        return asdict(self)


# ---- bridge: the state file the C++ widget polls -------------------------------
def state_file_path() -> Optional[str]:
    """Path of the bridge file, or None if the bridge is switched off."""
    override = os.environ.get(STATE_FILE_ENV)
    if override is not None:
        return override or None  # empty value = bridge off
    base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), ".local", "share")
    return os.path.join(base, "Oculi", "state.txt")


def _write_state_file(event: Event) -> None:
    """Atomically publish `event` as one line '<state> <timestamp> <source>'. Never raises."""
    path = state_file_path()
    if not path:
        return
    line = f"{event.state} {event.timestamp:.6f} {'_'.join(event.source.split())}\n"
    tmp = f"{path}.{os.getpid()}.tmp"
    try:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(line)
        for _ in range(5):
            try:
                os.replace(tmp, path)
                return
            except OSError:
                time.sleep(0.01)  # on Windows the reader may hold the file for a moment
    except OSError:
        pass
    try:
        os.remove(tmp)
    except OSError:
        pass


def read_state_file(path: Optional[str] = None) -> Optional[Event]:
    """Read the bridge file back (handy for tests and debugging). None if missing/invalid."""
    path = path or state_file_path()
    if not path:
        return None
    try:
        with open(path, encoding="utf-8") as f:
            parts = f.readline().split(None, 2)
        state, timestamp = parts[0], float(parts[1])
        source = parts[2].strip() if len(parts) > 2 else ""
    except (OSError, ValueError, IndexError):
        return None
    return Event(source, state, timestamp) if state in STATES else None


class Oculi:
    """Receives events from any number of sources and exposes the current state."""

    def __init__(self, clock: Callable[[], float] = time.time, history: int = 100, bridge: bool = True):
        self._clock = clock
        self._bridge = bridge  # publish every event to the state file
        self._lock = threading.Lock()
        self._history: Deque[Event] = deque(maxlen=history)  # recent events, oldest first
        self._by_source: Dict[str, Event] = {}               # latest event per source
        self._latest: Optional[Event] = None

    # ---- input -----------------------------------------------------------
    def send(self, source: str, state: str) -> Event:
        """Record an event. Raises InvalidStateError / ValueError on bad input."""
        if not isinstance(source, str) or not source.strip():
            raise ValueError("source must be a non-empty string")
        if not isinstance(state, str) or state.strip().lower() not in STATES:
            raise InvalidStateError(f"invalid state {state!r}; expected one of {STATES}")
        event = Event(source.strip().lower(), state.strip().lower(), self._clock())
        with self._lock:
            self._history.append(event)
            self._by_source[event.source] = event
            self._latest = event
            if self._bridge:
                _write_state_file(event)  # inside the lock so the file always holds the latest event
        return event

    # ---- output ----------------------------------------------------------
    def _resolve(self) -> Optional[Event]:
        """Pick the event that decides the eye's look.

        Today: simply the most recent event. Priority/context rules (for example
        error > warning > working, or per-source expiry) belong here later, so
        nothing else has to change.
        """
        return self._latest

    def current_event(self) -> Optional[Event]:
        with self._lock:
            return self._resolve()

    def current_state(self) -> str:
        event = self.current_event()
        if event is None:
            return DEFAULT_STATE
        if event.state == "success" and self._clock() - event.timestamp >= SUCCESS_SECONDS:
            return DEFAULT_STATE  # success is brief; the C++ widget applies the same rule
        return event.state

    def visual(self) -> Visual:
        return VISUALS[self.current_state()]

    def state_of(self, source: str) -> Optional[str]:
        """Latest state reported by one source, or None if it never reported."""
        with self._lock:
            event = self._by_source.get(source.strip().lower())
        return event.state if event else None

    def sources(self) -> Dict[str, Event]:
        """Latest event for every source that has reported."""
        with self._lock:
            return dict(self._by_source)

    def events(self) -> List[Event]:
        """Recent events, oldest first."""
        with self._lock:
            return list(self._history)

    def reset(self) -> None:
        with self._lock:
            self._history.clear()
            self._by_source.clear()
            self._latest = None
            if self._bridge:
                _write_state_file(Event("reset", "idle", self._clock()))


# ---- module-level API: `oculi.send(...)` -----------------------------------
_default = Oculi()


def send(source: str, state: str) -> Event:
    return _default.send(source, state)


def current_event() -> Optional[Event]:
    return _default.current_event()


def current_state() -> str:
    return _default.current_state()


def visual() -> Visual:
    return _default.visual()


def state_of(source: str) -> Optional[str]:
    return _default.state_of(source)


def sources() -> Dict[str, Event]:
    return _default.sources()


def events() -> List[Event]:
    return _default.events()


def reset() -> None:
    _default.reset()


# ---- run any command and report its outcome -------------------------------------
RUN_USAGE = "python -m oculi run --source NAME -- COMMAND [ARGS...]"


def run_command(source: str, command: List[str]) -> int:
    """send working -> run `command` -> send success/error. Returns the command's exit code.

    The command inherits our stdin/stdout/stderr, so its output and prompts pass through untouched.
    """
    command = list(command)
    name = command[0]
    send(source, "working")  # raises ValueError for a bad source, before anything is executed
    try:
        command[0] = shutil.which(name) or name  # lets Windows find npm.cmd, pytest.exe, ...
        code = subprocess.call(command)
    except KeyboardInterrupt:
        send(source, "error")
        return 130
    except OSError as exc:  # not found / not executable
        print(f"oculi: cannot run {name!r}: {exc}", file=sys.stderr)
        send(source, "error")
        return 126 if isinstance(exc, PermissionError) else 127
    except BaseException:
        send(source, "error")  # never leave the eye stuck on "working"
        raise
    if code < 0:  # killed by a signal (POSIX): use the shell convention 128 + signal
        code = 128 - code
    send(source, "success" if code == 0 else "error")
    return code


def _demo() -> None:  # python oculi.py
    for src, st in [("training", "working"), ("tests", "warning"), ("training", "success"), ("claude", "error")]:
        e = send(src, st)
        print(f"{e.source:<9} -> {e.state:<8} now: {current_state():<8} ({visual().description})")


def main(argv: Optional[List[str]] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        _demo()
        return 0
    if argv[0] != "run":
        print(f"oculi: unknown command {argv[0]!r}\nusage: {RUN_USAGE}", file=sys.stderr)
        return 2
    rest = argv[1:]
    if "--" not in rest:
        print(f"oculi: put '--' before the command\nusage: {RUN_USAGE}", file=sys.stderr)
        return 2
    cut = rest.index("--")  # only the first '--' splits; later ones belong to the command
    options, command = rest[:cut], rest[cut + 1:]
    parser = argparse.ArgumentParser(prog="python -m oculi run", usage=RUN_USAGE)
    parser.add_argument("--source", required=True, help="name of what is being watched, e.g. training")
    try:
        args = parser.parse_args(options)
    except SystemExit as exc:  # argparse already printed the message
        return exc.code if isinstance(exc.code, int) else 2
    if not command:
        print(f"oculi: no command given after '--'\nusage: {RUN_USAGE}", file=sys.stderr)
        return 2
    try:
        return run_command(args.source, command)
    except ValueError as exc:  # e.g. empty --source
        print(f"oculi: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
