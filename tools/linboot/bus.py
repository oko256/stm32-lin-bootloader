"""LIN bus access for the host tool.

The update logic (updater.py, provider.py) only uses these interfaces, so that it can run on a USB
LIN Card as well as against the simulated bootloader in the tests.
"""

import time
from abc import ABC, abstractmethod

from usblincard import BAUD_PRESETS, UsbLinCard, R0, r1, t1


class Clock:
    """Real time; the simulated buses replace this with a virtual clock."""

    def time(self) -> float:
        return time.monotonic()

    def sleep(self, seconds: float) -> None:
        time.sleep(seconds)


class CommanderBus(Clock, ABC):
    """The host is the bus commander (updating a RESPONDER role bootloader)."""

    @abstractmethod
    def send_frame(self, lin_id: int, data: bytes) -> None:
        """Sends a whole frame: header and our own response."""

    @abstractmethod
    def request(self, lin_id: int, length: int, timeout: float) -> bytes | None:
        """Sends a header and returns the response of the responder, or None if there was none
        within timeout (polling repeatedly meanwhile is allowed)."""


class ResponderBus(Clock, ABC):
    """The host is a responder on the bus (a provider for a COMMANDER role bootloader)."""

    @abstractmethod
    def configure(self, subscribe: dict[int, int], publish: dict[int, bytes]) -> None:
        """Replaces the responder table: IDs to subscribe to (with their lengths), and the responses
        to publish."""

    @abstractmethod
    def receive(self, timeout: float) -> tuple[int, bytes] | None:
        """Returns the next received (subscribed) frame as (id, data), or None on timeout."""


def _baud_command(baud: int) -> str:
    for cmd, preset in BAUD_PRESETS.items():
        if preset == baud:
            return cmd
    raise ValueError(f"the USB LIN Card doesn't support {baud} baud (only {sorted(BAUD_PRESETS.values())})")


def _open_card(card: UsbLinCard, baud: int, classic_checksum: bool) -> None:
    card.cmd("C")  # may fail if already closed
    card.cmd_ok("w")
    card.cmd_ok(_baud_command(baud))
    card.cmd_ok("x2" if classic_checksum else "x1")
    card.cmd_ok("O")
    card.flags()  # clear old errors


class CardCommanderBus(CommanderBus):
    """CommanderBus on a USB LIN Card (with the stock firmware).

    Frames are sent with single frames ("t0"). The card has no single header command, so a request
    temporarily schedules the header ("R0"), which repeats every slot until the response arrives,
    and is then wiped again.
    """

    # Slot of the scheduled header for requests; the bootloader may not respond while it's writing
    # flash, so the header simply repeats.
    REQUEST_SLOT_MS = 20

    def __init__(self, card: UsbLinCard, baud: int = 19200, classic_checksum: bool = False):
        self.card = card
        _open_card(card, baud, classic_checksum)

    def send_frame(self, lin_id: int, data: bytes) -> None:
        self.card.send_frame(lin_id, data)

    def request(self, lin_id: int, length: int, timeout: float) -> bytes | None:
        self.card.clear_frames()
        self.card.cmd_ok(R0(lin_id, self.REQUEST_SLOT_MS, length))
        try:
            deadline = time.monotonic() + timeout
            while True:
                frame = self.card.pop_frame(max(0.0, deadline - time.monotonic()))
                if frame is None:
                    return None
                if frame.id == lin_id and len(frame.data) == length:
                    return frame.data
        finally:
            self.card.cmd_ok("w")


class CardResponderBus(ResponderBus):
    """ResponderBus on a USB LIN Card (with the stock firmware).

    The card can't change a single table entry, so every change rewrites the whole table. The card
    doesn't report the frames it publishes itself either: a provider can only change its DATA
    response when it gets a READ (the bootloader handles that).
    """

    def __init__(self, card: UsbLinCard, baud: int = 19200, classic_checksum: bool = False):
        self.card = card
        _open_card(card, baud, classic_checksum)

    def configure(self, subscribe: dict[int, int], publish: dict[int, bytes]) -> None:
        self.card.cmd_ok("w")
        for lin_id, length in subscribe.items():
            self.card.cmd_ok(r1(lin_id, length))
        for lin_id, data in publish.items():
            self.card.cmd_ok(t1(lin_id, data))

    def receive(self, timeout: float) -> tuple[int, bytes] | None:
        frame = self.card.pop_frame(timeout)
        return None if frame is None else (frame.id, frame.data)
