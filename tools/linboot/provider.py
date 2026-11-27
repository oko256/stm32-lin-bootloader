"""Provider: the host is a responder that provides the update for a COMMANDER role bootloader."""

from dataclasses import dataclass, field
from typing import Callable

import protocol as p
from bus import ResponderBus
from stream import ChunkStream


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
        # The end of the stream has been read (READ for it, or when streaming, published).
        self.read_end = False
        self.streaming = bus.response_queue_depth > 0
        self.chunks: ChunkStream | None = None

    def _apply(self) -> None:
        publish = {}
        if self.offering:
            publish[self.ids.offers[self.index]] = self.offer
            if self.data is not None:
                publish[self.ids.data] = self.data
        self.bus.configure({self.ids.query: p.FRAME_SIZE}, publish)

    def start(self) -> None:
        self._apply()

    def _matches(self, identify: p.Identify) -> bool:
        return (self.device_type is None or identify.device_type == self.device_type) and (
            self.hardware_version is None or identify.hardware_version == self.hardware_version
        )

    def _stream_from(self, offset: int) -> None:
        """Serves DATA from @p offset on, through the bus's response queue."""
        if self.data is None:
            # Add the DATA response to the table first.
            self.data = p.chunk_frame(self.stream, offset)
            self._apply()
            self.chunks = ChunkStream(self.bus, self.ids.data, self.stream)
        self.chunks.restart(offset, len(self.stream))

    def _published(self, data: bytes) -> None:
        """The bus reports that our DATA response @p data was published."""
        offset = self.chunks.published(data)
        if offset is None:
            return
        if offset + p.CHUNK_SIZE >= len(self.stream):
            self.read_end = True
        if self.progress is not None:
            self.progress(min(offset + p.CHUNK_SIZE, len(self.image)), len(self.image))

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

    def run(self, timeout: float | None = None, until_done: bool = True, quiet_timeout: float = 2.0) -> int | None:
        """Serves until the bootloader reports a successful result (if until_done), or until timeout.
        Returns the last result (error code), or None if there was none.

        The RESULT frames may be missed (e.g. while the responder table is being rewritten without a
        response queue), so if the bus goes quiet for quiet_timeout after the end of the stream was
        read, the update is assumed to be done (the bootloader starts the application right after a
        successful update)."""
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
