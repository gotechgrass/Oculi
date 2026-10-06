"""oculi.py - the event/state layer for Oculi.

Python only tracks *what Oculi should be feeling*; it does not draw anything.
Any program can report its status with:

    import oculi
    oculi.send(source="training", state="working")
    oculi.send(source="training", state="success")

Standard library only. Later, the C++ renderer can read `current_state()` /
`visual()` (or `Event.to_dict()`) to decide how the eye should look.
"""
import threading
import time
from collections import deque
from dataclasses import asdict, dataclass
from typing import Callable, Deque, Dict, List, Optional

STATES = ("idle", "working", "success", "warning", "error")
DEFAULT_STATE = "idle"

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


class Oculi:
    """Receives events from any number of sources and exposes the current state."""

    def __init__(self, clock: Callable[[], float] = time.time, history: int = 100):
        self._clock = clock
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
        return event.state if event else DEFAULT_STATE

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


if __name__ == "__main__":  # tiny demo: python oculi.py
    for src, st in [("training", "working"), ("tests", "warning"), ("training", "success"), ("claude", "error")]:
        e = send(src, st)
        print(f"{e.source:<9} -> {e.state:<8} now: {current_state():<8} ({visual().description})")
