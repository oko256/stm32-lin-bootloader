"""LIN bus access for the host tool.

The update logic (updater.py, provider.py) only uses these interfaces, so that it can run on a USB
LIN Card as well as against the simulated bootloader in the tests. The card is used through the
usb-lin-card library (see requirements.txt).
"""

import math
import time
from abc import ABC, abstractmethod

from usblincard import Checksum, UsbLinCard
from usblincard.protocol import RESPONSE_QUEUE_DEPTH, TABLE_SIZE


class Clock:
    """Real time; the simulated buses replace this with a virtual clock."""

    def time(self) -> float:
        return time.monotonic()

    def sleep(self, seconds: float) -> None:
        time.sleep(seconds)


class ResponseQueue:
    """Optional support for changing one published response in place (replace_response()) and
    queuing the next ones (queue_response()); response_queue_depth is then non-zero. The publishes
    of a response changed that way are reported by receive(), like received frames."""

    #: How many responses queue_response() can hold (0: neither it nor replace_response() is supported).
    response_queue_depth = 0

    def replace_response(self, lin_id: int, data: bytes) -> None:
        """Replaces the published response of @p lin_id (from the next header on), and clears its
        queued responses."""
        raise NotImplementedError

    def queue_response(self, lin_id: int, data: bytes) -> bool:
        """Queues a response of @p lin_id to be published after the current one. Returns False if
        the queue is full."""
        raise NotImplementedError


class CommanderBus(Clock, ResponseQueue, ABC):
    """The host is the bus commander (updating a RESPONDER role bootloader).

    With a response queue, the bus can also stream frames (start_stream()): it sends frames whose
    response comes from the queue back to back, without a round trip to the host for each one."""

    @abstractmethod
    def send_frame(self, lin_id: int, data: bytes) -> None:
        """Sends a whole frame: header and our own response."""

    @abstractmethod
    def request(self, lin_id: int, length: int, timeout: float) -> bytes | None:
        """Sends a header and returns the response of the responder, or None if there was none
        within timeout (polling repeatedly meanwhile is allowed)."""

    def start_stream(self, lin_id: int, data: bytes, polls: dict[int, int]) -> None:
        """Sends frames of @p lin_id with our own response (initially @p data, then changed with
        replace_response() and queue_response()) over and over, with the headers of @p polls (IDs
        and their response lengths) in between, until stop_stream(). The publishes and the poll
        responses are reported by receive(), in bus order."""
        raise NotImplementedError

    def stop_stream(self) -> None:
        raise NotImplementedError

    def receive(self, timeout: float) -> tuple[int, bytes] | None:
        """Returns the next reported frame of the stream as (id, data), or None on timeout."""
        raise NotImplementedError


class ResponderBus(Clock, ResponseQueue, ABC):
    """The host is a responder on the bus (a provider for a COMMANDER role bootloader)."""

    @abstractmethod
    def configure(self, subscribe: dict[int, int], publish: dict[int, bytes]) -> None:
        """Replaces the responder table: IDs to subscribe to (with their lengths), and the responses
        to publish."""

    @abstractmethod
    def receive(self, timeout: float) -> tuple[int, bytes] | None:
        """Returns the next received (subscribed) or reported (published) frame as (id, data), or
        None on timeout."""


def stream_slots_ms(baud: int) -> tuple[int, int]:
    """Slots (ms) of the stream's frames and polls on the USB LIN Card.

    A frame (header and the card's own response) takes 124 bit times. Its slot is a bit shorter, so
    that the frames follow each other back to back: the card's transmits are blocking, so the next
    header waits until the response has been sent, as long as the slot doesn't end before the card
    has started its response. A poll gets the bootloader's own frame slot (LINBOOT_SLOT_MS), as the
    response comes from the bootloader.
    """
    frame_ms = math.ceil(115 * 1000 / baud)
    poll_ms = math.ceil(173.6 * 1000 / baud) + 1
    return frame_ms, poll_ms


class _CardBus:
    """Common part of the buses on a USB LIN Card: opens the card's channel, and uses its response
    queue ("t2", "t3"), which card firmware 6 and later has."""

    def __init__(self, card: UsbLinCard, baud: int = 19200, classic_checksum: bool = False):
        self.card = card
        card.reset(baud, Checksum.CLASSIC if classic_checksum else Checksum.ENHANCED)
        card.open_channel()
        card.read_flags()  # clear old errors
        if card.supports_response_queue:
            self.response_queue_depth = RESPONSE_QUEUE_DEPTH

    def replace_response(self, lin_id: int, data: bytes) -> None:
        self.card.replace_response(lin_id, data)

    def queue_response(self, lin_id: int, data: bytes) -> bool:
        return self.card.queue_response(lin_id, data)

    def receive(self, timeout: float) -> tuple[int, bytes] | None:
        frame = self.card.receive(timeout)
        return None if frame is None else (frame.id, frame.data)


class CardCommanderBus(_CardBus, CommanderBus):
    """CommanderBus on a USB LIN Card.

    Frames are sent with single frames ("t0"). The card has no single header command, so a request
    (UsbLinCard.request()) temporarily schedules the header ("R0"), which repeats every slot until
    the response arrives, and is then wiped again. A stream is a schedule of command frames ("T0", whose response the
    response queue changes) and polled headers ("R0").
    """

    # Slot of the scheduled header for requests; the bootloader may not respond while it's writing
    # flash, so the header simply repeats.
    REQUEST_SLOT_MS = 20
    # Stream: the frames in the schedule before each round of polls.
    STREAM_FRAMES = 16

    def __init__(self, card: UsbLinCard, baud: int = 19200, classic_checksum: bool = False):
        super().__init__(card, baud, classic_checksum)
        self.stream_frame_slot_ms, self.stream_poll_slot_ms = stream_slots_ms(baud)

    def send_frame(self, lin_id: int, data: bytes) -> None:
        self.card.send_frame(lin_id, data)

    def request(self, lin_id: int, length: int, timeout: float) -> bytes | None:
        return self.card.request(lin_id, length, timeout, slot_ms=self.REQUEST_SLOT_MS)

    def start_stream(self, lin_id: int, data: bytes, polls: dict[int, int]) -> None:
        if self.STREAM_FRAMES + len(polls) > TABLE_SIZE:
            raise ValueError(f"at most {TABLE_SIZE - self.STREAM_FRAMES} polls fit in the card's schedule")
        # The card answers all the frames of lin_id with the response of the first one, which is
        # what the response queue changes.
        self.card.wipe()
        self.card.clear_frames()
        for _ in range(self.STREAM_FRAMES):
            self.card.schedule_frame(lin_id, data, self.stream_frame_slot_ms)
        for poll_id, length in polls.items():
            self.card.schedule_header(poll_id, length, self.stream_poll_slot_ms)

    def stop_stream(self) -> None:
        self.card.wipe()


class CardResponderBus(_CardBus, ResponderBus):
    """ResponderBus on a USB LIN Card. Without the response queue (card firmware older than 6), the
    card doesn't report its publishes: a provider then changes its DATA response only when it gets
    a READ."""

    def configure(self, subscribe: dict[int, int], publish: dict[int, bytes]) -> None:
        self.card.wipe()
        for lin_id, length in subscribe.items():
            self.card.subscribe(lin_id, length)
        for lin_id, data in publish.items():
            self.card.publish(lin_id, data)
