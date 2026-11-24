"""Buses that connect the host tool to the simulated bootloader (test/sim/linboot_sim.c, built by
the unit test project), with a virtual clock.

The simulated bootloader uses the unit test configuration (test/stubs/linboot_config.h): RESPONDER
IDs 0x30/0x31/0x32, COMMANDER IDs 0x31 (QUERY), 0x32 (DATA) and 0x33..0x35 (OFFER).
"""

import ctypes
import os
import random
from pathlib import Path

from bus import CommanderBus, ResponderBus

REPO = Path(__file__).resolve().parents[3]
DEFAULT_LIB = REPO / "test" / "build" / "liblinboot_sim.so"

DEVICE_TYPE = 0x1234
HW_VERSION = 0x0003
OFFER_IDS = [0x33, 0x34, 0x35]

# Bus time of one frame (ms).
FRAME_MS = 10

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
    """Common part: the simulated flash and the virtual clock (ms)."""

    def __init__(self, lib: ctypes.CDLL, app_size: int = 0, version: int = 0, loss: float = 0.0, seed: int = 1):
        self.lib = lib
        self.now = 1000
        self.booted = False
        self.loss = loss
        self.rng = random.Random(seed)
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


class SimResponderBus(SimDevice, CommanderBus):
    """The host is the commander of a simulated RESPONDER role bootloader. Frames can be lost."""

    def _poll(self):
        if self.lib.sim_responder_poll(self.now) != 0:
            self.booted = True

    def sleep(self, seconds: float) -> None:
        self.now += max(1, int(seconds * 1000))
        self._poll()

    def send_frame(self, lin_id: int, data: bytes) -> None:
        assert len(data) == 8
        self.now += FRAME_MS
        if not self.booted and not self.lost():
            self.lib.sim_responder_frame(lin_id, _buf(data), self.now)
        self._poll()

    def request(self, lin_id: int, length: int, timeout: float) -> bytes | None:
        # Polls like the USB LIN Card: the header repeats until there is a response.
        deadline = self.now + int(timeout * 1000)
        while True:
            self.now += FRAME_MS
            out = _buf()
            got = not self.booted and self.lib.sim_responder_publish(lin_id, out, self.now) and not self.lost()
            self._poll()
            if got:
                return bytes(out)[:length]
            if self.now >= deadline:
                return None


class SimCommanderBus(SimDevice, ResponderBus):
    """The host is a responder (provider) for a simulated COMMANDER role bootloader. Our responses
    can be lost."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.subscribe: dict[int, int] = {}
        self.publish: dict[int, bytes] = {}
        self.received: list[tuple[int, bytes]] = []
        self.headers: list[int] = []
        # Keep a reference: the callback must outlive its use from C.
        self._callback = HEADER_CALLBACK(self._on_header)
        self.lib.sim_commander_set_header_callback(self._callback)

    def _on_header(self, lin_id: int) -> None:
        self.headers.append(lin_id)
        out = _buf()
        if self.lib.sim_commander_publish(lin_id, out, self.now):
            # The bootloader's own frame (QUERY).
            if lin_id in self.subscribe:
                self.received.append((lin_id, bytes(out)))
        elif lin_id in self.publish and not self.lost():
            self.lib.sim_commander_frame(lin_id, _buf(self.publish[lin_id]), self.now)

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

    def receive(self, timeout: float) -> tuple[int, bytes] | None:
        end = self.now + int(timeout * 1000)
        while not self.received and self.now < end and not self.booted:
            self._step()
        if self.received:
            return self.received.pop(0)
        if self.booted:
            self.now = end
        return None
