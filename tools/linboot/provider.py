"""Provider: the host is a responder that provides the update for a COMMANDER role bootloader."""

from collections import deque
from dataclasses import dataclass, field
from typing import Callable

import protocol as p
from bus import ResponderBus


@dataclass
class CommanderIds:
    """Frame IDs of the COMMANDER role bootloader (LINBOOT_ID_QUERY, _DATA and
    LINBOOT_PROVIDER_OFFER_IDS)."""

    query: int = 0x31
    data: int = 0x32
    offers: list[int] = field(default_factory=lambda: [0x33, 0x34, 0x35, 0x36])


class Provider:
    """Offers an image in the OFFER frame of provider @p index, and serves the DATA frame from the
    offset of the last READ.

    If the bus can queue responses (ResponderBus.response_queue_depth), the chunks after the READ
    offset are queued, and the queue is kept full as the bus reports the published ones: the DATA
    frames advance by themselves, like with an MCU provider. Otherwise the DATA response changes
    only on a READ, so the bootloader sends a READ for (nearly) every chunk."""

    def __init__(
        self,
        bus: ResponderBus,
        image: bytes,
        version: int,
        force: bool = False,
        index: int = 0,
        ids: CommanderIds = CommanderIds(),
        device_type: int | None = None,
        hardware_version: int | None = None,
        log: Callable[[str], None] = print,
        progress: Callable[[int, int], None] | None = None,
    ):
        if not 0 <= index < len(ids.offers):
            raise ValueError(f"provider index {index} out of range (0..{len(ids.offers) - 1})")
        self.bus = bus
        self.image = image
        self.stream = p.provider_stream(image)
        self.offer = p.offer_frame(version, len(image), force)
        self.index = index
        self.ids = ids
        self.device_type = device_type
        self.hardware_version = hardware_version
        self.log = log
        self.progress = progress
        self.offering = False
        self.data: bytes | None = None
        self.last_identify: p.Identify | None = None
        self.result: int | None = None
        # The last READ was for the end of the stream (or, when streaming, it was published).
        self.read_end = False
        # Streaming: the chunk offsets given to the bus and not published yet; the first one is the
        # current response.
        self.streaming = bus.response_queue_depth > 0
        self.in_bus: deque[int] = deque()
        self.next_offset = 0

    def _apply(self) -> None:
        self.in_bus.clear()
        publish = {}
        if self.offering:
            publish[self.ids.offers[self.index]] = self.offer
            if self.data is not None:
                publish[self.ids.data] = self.data
        self.bus.configure({self.ids.query: p.FRAME_SIZE}, publish)

    def start(self) -> None:
        self._apply()

    def _matches(self, identify: p.Identify) -> bool:
        if self.device_type is not None and identify.device_type != self.device_type:
            return False
        if self.hardware_version is not None and identify.hardware_version != self.hardware_version:
            return False
        return True

    def _stream_from(self, offset: int) -> None:
        """Serves DATA from @p offset on: the chunk at it is the current response, and the next ones
        are queued."""
        data = p.chunk_frame(self.stream, offset)
        if self.data is None:
            # Add the DATA response to the table first.
            self.data = data
            self._apply()
        # Replacing also makes the bus report the publishes (even if nothing gets queued).
        self.data = data
        self.bus.replace_response(self.ids.data, data)
        self.in_bus.clear()
        self.in_bus.append(offset)
        self.next_offset = offset + p.CHUNK_SIZE
        self._fill()

    def _fill(self) -> None:
        while len(self.in_bus) <= self.bus.response_queue_depth and self.next_offset < len(self.stream):
            if not self.bus.queue_response(self.ids.data, p.chunk_frame(self.stream, self.next_offset)):
                break
            self.in_bus.append(self.next_offset)
            self.next_offset += p.CHUNK_SIZE

    def _published(self, data: bytes) -> None:
        """The bus reports that our DATA response @p data was published."""
        if not self.in_bus or data != p.chunk_frame(self.stream, self.in_bus[0]):
            # Published before we replaced the response.
            return
        offset = self.in_bus[0]
        if len(self.in_bus) > 1:
            # The next queued chunk is now the current response.
            self.in_bus.popleft()
        if offset + p.CHUNK_SIZE >= len(self.stream):
            self.read_end = True
        if self.progress is not None:
            self.progress(min(offset + p.CHUNK_SIZE, len(self.image)), len(self.image))
        self._fill()

    def handle_frame(self, lin_id: int, data: bytes) -> None:
        if lin_id == self.ids.data and self.streaming and self.data is not None:
            self._published(data)
            return
        if lin_id != self.ids.query:
            return
        code, details = p.decode_query(data)
        if code == p.Query.IDENTIFY:
            if details != self.last_identify:
                self.last_identify = details
                self.log(
                    f"Bootloader: device type 0x{details.device_type:04X}, hardware 0x{details.hardware_version:04X}, "
                    f"firmware {p.format_version(details.firmware_version)}"
                    f"{'' if details.app_valid else ' (no valid application)'}"
                )
            offering = self._matches(details)
            # A new discovery: whatever transfer there was is over, so stop serving DATA too.
            if offering != self.offering or self.data is not None:
                self.offering = offering
                self.data = None
                self.read_end = False
                if not offering:
                    self.log("Not a matching device: not offering the image")
                self._apply()
        elif code == p.Query.READ:
            provider, offset = details
            if provider != self.index:
                # Another provider was selected: DATA is now its frame, and responding to it too
                # would collide on the bus.
                if self.data is not None:
                    self.data = None
                    self.read_end = False
                    self._apply()
                return
            if not self.offering:
                return
            if offset == 0:
                self.log("Transfer started")
            if self.streaming:
                # Every READ restarts the stream: the queued chunks may have gone past the offset.
                self._stream_from(offset)
                return
            data = p.chunk_frame(self.stream, offset)
            if data == self.data:
                # A repeated READ (we were too slow): already serving it, and rewriting the table
                # again would only open another window for missing QUERY frames.
                return
            self.data = data
            self.read_end = offset + p.CHUNK_SIZE >= len(self.stream)
            self._apply()
            if self.progress is not None:
                self.progress(min(offset + p.CHUNK_SIZE, len(self.image)), len(self.image))
        elif code == p.Query.RESULT:
            provider, error = details
            if provider != self.index:
                return
            self.result = error
            self.data = None
            self._apply()
            if error == p.Error.NONE:
                self.log("Update done")
            else:
                self.log(f"Update failed: {p.error_name(error)}")

    def run(
        self, timeout: float | None = None, until_done: bool = True, quiet_timeout: float = 2.0
    ) -> int | None:
        """Serves until the bootloader reports a successful result (if until_done), or until timeout.
        Returns the last result (error code), or None if there was none.

        Without a response queue on the bus, the RESULT frames may be missed (while the responder
        table is being rewritten), so if the bus goes quiet for quiet_timeout after the end of the stream was read, the update is assumed
        to be done (the bootloader starts the application right after a successful update)."""
        self.start()
        deadline = None if timeout is None else self.bus.time() + timeout
        last_frame = self.bus.time()
        while deadline is None or self.bus.time() < deadline:
            frame = self.bus.receive(0.1)
            if frame is not None:
                last_frame = self.bus.time()
                self.handle_frame(*frame)
                if until_done and self.result == p.Error.NONE:
                    break
            elif self.read_end and self.result is None and self.bus.time() - last_frame >= quiet_timeout:
                self.log("No RESULT seen, but the whole image was read and the bootloader went quiet: assuming done")
                self.result = p.Error.NONE
                if until_done:
                    break
        return self.result
