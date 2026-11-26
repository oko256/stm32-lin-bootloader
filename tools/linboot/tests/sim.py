"""Buses that connect the host tool to the simulated bootloader (test/sim/linboot_sim.c, built by
the unit test project), with a virtual clock.

The simulated bootloader uses the unit test configuration (test/stubs/linboot_config.h): RESPONDER
IDs 0x30/0x31/0x32, COMMANDER IDs 0x31 (QUERY), 0x32 (DATA) and 0x33..0x35 (OFFER).
"""

import ctypes
import os
import random
from pathlib import Path

from bus import CardCommanderBus, CommanderBus, ResponderBus, stream_slots_ms
from usblincard import auto_slot_ms

REPO = Path(__file__).resolve().parents[3]
DEFAULT_LIB = REPO / "test" / "build" / "liblinboot_sim.so"

DEVICE_TYPE = 0x1234
HW_VERSION = 0x0003
OFFER_IDS = [0x33, 0x34, 0x35]

# Bus time (ms) of a single frame, a request header, and the frames and polls of a stream (like
# the USB LIN Card's slots at 19200 baud).
FRAME_MS = auto_slot_ms(8, 19200)
REQUEST_MS = CardCommanderBus.REQUEST_SLOT_MS
STREAM_FRAME_MS, STREAM_POLL_MS = stream_slots_ms(19200)

HEADER_CALLBACK = ctypes.CFUNCTYPE(None, ctypes.c_uint8)


def load_lib() -> ctypes.CDLL | None:
    path = Path(os.environ.get("LINBOOT_SIM_LIB", DEFAULT_LIB))
    if not path.exists():
        return None
    lib = ctypes.CDLL(str(path))
    u8p = ctypes.POINTER(ctypes.c_uint8)
    lib.sim_reset.argtypes = [ctypes.c_uint32, ctypes.c_uint16, ctypes.c_uint32]
    lib.sim_installed.argtypes = [u8p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint16)]
    lib.sim_installed.restype = ctypes.c_uint32
    lib.sim_app_max_size.restype = ctypes.c_uint32
    for role in ("responder", "commander"):
        getattr(lib, f"sim_{role}_frame").argtypes = [ctypes.c_uint8, u8p, ctypes.c_uint32]
        getattr(lib, f"sim_{role}_publish").argtypes = [ctypes.c_uint8, u8p, ctypes.c_uint32]
        getattr(lib, f"sim_{role}_publish").restype = ctypes.c_bool
        getattr(lib, f"sim_{role}_poll").argtypes = [ctypes.c_uint32]
        getattr(lib, f"sim_{role}_poll").restype = ctypes.c_int
    lib.sim_commander_set_header_callback.argtypes = [HEADER_CALLBACK]
    return lib


def _buf(data: bytes = bytes(8)):
    return (ctypes.c_uint8 * 8).from_buffer_copy(data)


class SimDevice:
    """Common part: the simulated flash, the virtual clock (ms), and lost frames.

    With queue_depth, the bus can replace and queue responses like the USB LIN Card firmware 6
    ("t2" and "t3"), and reports their publishes. Reported frames reach the host latency_ms after
    they were on the bus (USB latency).
    """

    def __init__(
        self,
        lib: ctypes.CDLL,
        app_size: int = 0,
        version: int = 0,
        loss: float = 0.0,
        seed: int = 1,
        queue_depth: int = 0,
        latency_ms: int = 0,
    ):
        self.lib = lib
        self.now = 1000
        self.booted = False
        self.loss = loss
        self.rng = random.Random(seed)
        self.response_queue_depth = queue_depth
        self.latency_ms = latency_ms
        self.publish: dict[int, bytes] = {}
        # Responses queued for queue_id; the IDs whose publishes are reported.
        self.queue: list[bytes] = []
        self.queue_id: int | None = None
        self.reported: set[int] = set()
        # (time the host gets it, id, data)
        self.received: list[tuple[int, int, bytes]] = []
        self.headers: list[int] = []
        lib.sim_reset(app_size, version, self.now)

    def time(self) -> float:
        return self.now / 1000

    def lost(self) -> bool:
        return self.loss > 0 and self.rng.random() < self.loss

    def installed(self) -> tuple[bytes, int] | None:
        out = (ctypes.c_uint8 * self.lib.sim_app_max_size())()
        version = ctypes.c_uint16()
        size = self.lib.sim_installed(out, len(out), ctypes.byref(version))
        return None if size == 0 else (bytes(out)[:size], version.value)

    def _step(self) -> None:
        """Advances the virtual time by one step (and runs the bootloader)."""
        raise NotImplementedError

    def _report(self, lin_id: int, data: bytes) -> None:
        self.received.append((self.now + self.latency_ms, lin_id, data))

    def _published(self, lin_id: int) -> None:
        """Our response of @p lin_id was published: report it and move the next queued one in."""
        if lin_id in self.reported:
            self._report(lin_id, self.publish[lin_id])
            if self.queue and self.queue_id == lin_id:
                self.publish[lin_id] = self.queue.pop(0)

    def replace_response(self, lin_id: int, data: bytes) -> None:
        assert self.response_queue_depth > 0 and lin_id in self.publish
        self.publish[lin_id] = data
        self.reported.add(lin_id)
        if self.queue_id == lin_id:
            self.queue = []

    def queue_response(self, lin_id: int, data: bytes) -> bool:
        assert self.response_queue_depth > 0 and lin_id in self.publish
        if len(self.queue) >= self.response_queue_depth or (self.queue and self.queue_id != lin_id):
            return False
        self.queue.append(data)
        self.queue_id = lin_id
        self.reported.add(lin_id)
        return True

    def _ready(self) -> bool:
        return bool(self.received) and self.received[0][0] <= self.now

    def receive(self, timeout: float) -> tuple[int, bytes] | None:
        end = self.now + int(timeout * 1000)
        while not self._ready() and self.now < end and not self.booted:
            self._step()
        # Once booted, time doesn't advance any more: deliver what's still on its way.
        if self._ready() or (self.booted and self.received):
            _, lin_id, data = self.received.pop(0)
            return lin_id, data
        if self.booted:
            self.now = end
        return None


class SimResponderBus(SimDevice, CommanderBus):
    """The host is the commander of a simulated RESPONDER role bootloader. Frames can be lost.

    A stream runs a schedule like the USB LIN Card's (CardCommanderBus.start_stream())."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.schedule: list[int] = []
        self.schedule_index = 0

    def _poll(self):
        if self.lib.sim_responder_poll(self.now) != 0:
            self.booted = True

    def _step(self) -> None:
        if not self.schedule:
            self.now += 1
            self._poll()
            return
        lin_id = self.schedule[self.schedule_index]
        self.schedule_index = (self.schedule_index + 1) % len(self.schedule)
        self.headers.append(lin_id)
        if lin_id in self.publish:
            self.now += STREAM_FRAME_MS
            if not self.booted and not self.lost():
                self.lib.sim_responder_frame(lin_id, _buf(self.publish[lin_id]), self.now)
            self._published(lin_id)
        else:
            self.now += STREAM_POLL_MS
            out = _buf()
            if not self.booted and self.lib.sim_responder_publish(lin_id, out, self.now) and not self.lost():
                self._report(lin_id, bytes(out))
        self._poll()

    def sleep(self, seconds: float) -> None:
        end = self.now + max(1, int(seconds * 1000))
        while self.now < end:
            self._step()

    def send_frame(self, lin_id: int, data: bytes) -> None:
        assert len(data) == 8 and not self.schedule
        self.headers.append(lin_id)
        self.now += FRAME_MS
        if not self.booted and not self.lost():
            self.lib.sim_responder_frame(lin_id, _buf(data), self.now)
        self._poll()

    def request(self, lin_id: int, length: int, timeout: float) -> bytes | None:
        # Polls like the USB LIN Card: the header repeats until there is a response.
        assert not self.schedule
        deadline = self.now + int(timeout * 1000)
        while True:
            self.headers.append(lin_id)
            self.now += REQUEST_MS
            out = _buf()
            got = not self.booted and self.lib.sim_responder_publish(lin_id, out, self.now) and not self.lost()
            self._poll()
            if got:
                return bytes(out)[:length]
            if self.now >= deadline:
                return None

    def start_stream(self, lin_id: int, data: bytes, polls: dict[int, int]) -> None:
        self.publish = {lin_id: data}
        self.queue = []
        self.reported = set()
        self.received = []
        self.schedule = [lin_id] * CardCommanderBus.STREAM_FRAMES + list(polls)
        self.schedule_index = 0

    def stop_stream(self) -> None:
        self.schedule = []
        self.publish = {}


class SimCommanderBus(SimDevice, ResponderBus):
    """The host is a responder (provider) for a simulated COMMANDER role bootloader. Our responses
    can be lost."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.subscribe: dict[int, int] = {}
        # The bootloader's QUERY frames.
        self.queries: list[bytes] = []
        # Keep a reference: the callback must outlive its use from C.
        self._callback = HEADER_CALLBACK(self._on_header)
        self.lib.sim_commander_set_header_callback(self._callback)

    def _on_header(self, lin_id: int) -> None:
        self.headers.append(lin_id)
        out = _buf()
        if self.lib.sim_commander_publish(lin_id, out, self.now):
            # The bootloader's own frame (QUERY).
            self.queries.append(bytes(out))
            if lin_id in self.subscribe:
                self._report(lin_id, bytes(out))
        elif lin_id in self.publish:
            if not self.lost():
                self.lib.sim_commander_frame(lin_id, _buf(self.publish[lin_id]), self.now)
            self._published(lin_id)

    def sleep(self, seconds: float) -> None:
        self.run_for(seconds)

    def run_for(self, seconds: float) -> None:
        end = self.now + int(seconds * 1000)
        while self.now < end and not self.booted:
            self._step()

    def _step(self):
        if self.lib.sim_commander_poll(self.now) != 0:
            self.booted = True
        self.now += 1

    def configure(self, subscribe: dict[int, int], publish: dict[int, bytes]) -> None:
        self.subscribe = dict(subscribe)
        self.publish = dict(publish)
        self.queue = []
        self.reported = set()
