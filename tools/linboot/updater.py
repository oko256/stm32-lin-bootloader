"""Updater: the host is the bus commander and updates a RESPONDER role bootloader."""

from collections.abc import Callable
from dataclasses import dataclass
from enum import Enum

import protocol as p
from bus import CommanderBus
from stream import ChunkStream


class UpdateError(Exception):
    pass


class Outcome(Enum):
    UPDATED = "updated"
    UP_TO_DATE = "up to date"


@dataclass
class ResponderIds:
    """Frame IDs of the RESPONDER role bootloader (LINBOOT_ID_STATUS, _CMD and _DATA)."""

    status: int = 0x30
    cmd: int = 0x31
    data: int = 0x32


class Updater:
    """Runs the update sequence: wait for the heartbeat response, OFFER, send the chunks up to the
    window the bootloader reports, FINISH, and BOOT.

    If the bus has a response queue (CommanderBus.response_queue_depth), the chunks are streamed:
    the bus sends the DATA frames back to back and polls the STATUS in between. Otherwise each
    chunk is a single frame, and the STATUS is requested after each window."""

    def __init__(
        self,
        bus: CommanderBus,
        ids: ResponderIds | None = None,
        attempts: int = 3,
        status_timeout: float = 0.1,
        stall_timeout: float = 3.0,
        log: Callable[[str], None] = print,
        progress: Callable[[int, int], None] | None = None,
        keep_alive: list[int] | None = None,
        keep_alive_interval: float = 1.0,
    ):
        """@p keep_alive: STATUS IDs of other bootloaders whose heartbeat is polled every
        @p keep_alive_interval meanwhile, so that they stay in their bootloaders (waiting for their
        turn) instead of starting their applications after the session timeout."""
        self.bus = bus
        self.ids = ids if ids is not None else ResponderIds()
        self.attempts = attempts
        self.status_timeout = status_timeout
        self.stall_timeout = stall_timeout
        self.log = log
        self.progress = progress
        self.keep_alive = keep_alive or []
        self.keep_alive_interval = keep_alive_interval
        self._last_keep_alive: float | None = None

    def read_status(self, timeout: float | None = None) -> p.Status | None:
        data = self.bus.request(self.ids.status, p.FRAME_SIZE, self.status_timeout if timeout is None else timeout)
        self._poll_keep_alive()
        return None if data is None else p.Status.decode(data)

    def _poll_keep_alive(self) -> None:
        if not self.keep_alive:
            return
        now = self.bus.time()
        if self._last_keep_alive is not None and now - self._last_keep_alive < self.keep_alive_interval:
            return
        self._last_keep_alive = now
        for status_id in self.keep_alive:
            self.bus.request(status_id, p.FRAME_SIZE, self.status_timeout)

    def _status_until(self, timeout: float) -> p.Status:
        """Polls STATUS until there is a response (the bootloader doesn't respond while it's busy
        writing the flash)."""
        deadline = self.bus.time() + timeout
        while True:
            status = self.read_status()
            if status is not None:
                return status
            if self.bus.time() >= deadline:
                raise UpdateError(f"no STATUS response within {timeout} s")

    def wait_for_device(self, timeout: float) -> p.Status:
        """Polls the heartbeat (STATUS) until the bootloader responds; it only does so for a while
        after a reset, so the device should be reset while this is waiting."""
        return self._status_until(timeout)

    def send_cmd(self, data: bytes) -> None:
        self.bus.send_frame(self.ids.cmd, data)

    def boot(self) -> None:
        self.send_cmd(p.cmd_simple(p.Cmd.BOOT))

    def update(
        self,
        image: bytes,
        version: int,
        force: bool = False,
        wait_timeout: float = 60.0,
        device_type: int | None = None,
        hardware_version: int | None = None,
        boot: bool = True,
    ) -> Outcome:
        if not image:
            raise UpdateError("empty image")
        status = self.wait_for_device(wait_timeout)
        self.log(f"Bootloader found: {status.describe()}")
        if status.state in (p.State.IDLE, p.State.REJECTED):
            if device_type is not None and status.device_type != device_type:
                raise UpdateError(f"device type is 0x{status.device_type:04X}, expected 0x{device_type:04X}")
            if hardware_version is not None and status.hardware_version != hardware_version:
                raise UpdateError(
                    f"hardware version is 0x{status.hardware_version:04X}, expected 0x{hardware_version:04X}"
                )

        for attempt in range(1, self.attempts + 1):
            outcome = self._attempt(image, version, force)
            if outcome is not None:
                if boot:
                    self.boot()
                return outcome
            self.log(f"Attempt {attempt}/{self.attempts} failed")
        raise UpdateError(f"update failed after {self.attempts} attempts")

    def _attempt(self, image: bytes, version: int, force: bool) -> Outcome | None:
        """One update attempt; returns None if it failed in a way that can be retried."""
        size = len(image)
        self.send_cmd(p.cmd_offer(version, size, force))
        status = self._status_until(self.stall_timeout)
        if status.state == p.State.REJECTED:
            self.log(f"Version {p.format_version(version)} is already installed (use force to reinstall)")
            return Outcome.UP_TO_DATE
        if status.state == p.State.FAILED and status.error == p.Error.BAD_SIZE:
            raise UpdateError(f"the image ({size} bytes) doesn't fit in the device")

        send = self._stream_chunks if self.bus.response_queue_depth > 0 else self._send_chunks
        if not send(image, status):
            return None

        self.send_cmd(p.cmd_finish(p.crc32(image)))
        deadline = self.bus.time() + self.stall_timeout
        while True:
            status = self._status_until(self.stall_timeout)
            if status.state == p.State.DONE:
                self.log(f"Update to version {p.format_version(version)} done")
                return Outcome.UPDATED
            if status.state == p.State.FAILED:
                self.log(f"Update failed: {status.describe()}")
                return None
            if status.state not in (p.State.RECEIVING, p.State.VERIFYING) or self.bus.time() > deadline:
                self.log(f"Unexpected state after FINISH: {status.describe()}")
                return None
            if status.state == p.State.RECEIVING:
                # FINISH was lost on the bus (all the data is in: it was only sent after that).
                self.send_cmd(p.cmd_finish(p.crc32(image)))

    def _send_chunks(self, image: bytes, status: p.Status) -> bool:
        """Sends the chunks as single frames, up to the window of each STATUS. Returns True once the
        bootloader has received all of them."""
        size = len(image)
        progress = _Progress(self, size)
        while progress.update(status):
            if status.next_offset >= size:
                return True
            for offset in range(status.next_offset, _window(status, size), p.CHUNK_SIZE):
                self.bus.send_frame(self.ids.data, p.chunk_frame(image, offset))
            status = self._status_until(self.stall_timeout)
        return False

    def _stream_chunks(self, image: bytes, status: p.Status) -> bool:
        """Streams the chunks through the bus's response queue, up to the window of the latest
        STATUS, which the bus polls between the DATA frames (and the heartbeats of keep_alive).
        Returns True once the bootloader has received all of them."""
        size = len(image)
        progress = _Progress(self, size)
        polls = {status_id: p.FRAME_SIZE for status_id in [self.ids.status, *self.keep_alive]}
        self.bus.start_stream(self.ids.data, p.chunk_frame(image, status.next_offset), polls)
        try:
            chunks = ChunkStream(self.bus, self.ids.data, image)
            chunks.restart(status.next_offset, _window(status, size))
            # End of the chunks published since the last restart: the bus reports in bus order, so a
            # STATUS reported after them reflects them.
            sent_end = status.next_offset
            while progress.check():
                frame = self.bus.receive(0.1)
                if frame is None:
                    continue
                lin_id, data = frame
                if lin_id == self.ids.data:
                    offset = chunks.published(data)
                    if offset is not None:
                        sent_end = offset + p.CHUNK_SIZE
                elif lin_id == self.ids.status:
                    status = p.Status.decode(data)
                    if not progress.update(status):
                        return False
                    if status.next_offset >= size:
                        return True
                    if status.next_offset < sent_end:
                        # A chunk was lost: the bootloader ignores the ones after it.
                        chunks.restart(status.next_offset, _window(status, size))
                        sent_end = status.next_offset
                    else:
                        chunks.extend(_window(status, size))
            return False
        finally:
            self.bus.stop_stream()


def _window(status: p.Status, size: int) -> int:
    return min(status.window_end, size)


class _Progress:
    """Follows the next_offset of the STATUS: reports the progress, and notices a stalled transfer."""

    def __init__(self, updater: Updater, size: int):
        self.updater = updater
        self.size = size
        self.last_next = -1
        self.last_time = updater.bus.time()

    def update(self, status: p.Status) -> bool:
        """Returns False if the transfer has stalled or was interrupted (and logs why)."""
        updater = self.updater
        if status.state != p.State.RECEIVING:
            updater.log(f"Transfer interrupted: {status.describe()}")
            return False
        if status.next_offset != self.last_next:
            self.last_next = status.next_offset
            self.last_time = updater.bus.time()
            if updater.progress is not None:
                updater.progress(min(status.next_offset, self.size), self.size)
        return self.check()

    def check(self) -> bool:
        if self.updater.bus.time() - self.last_time > self.updater.stall_timeout:
            self.updater.log("Transfer stalled")
            return False
        return True


@dataclass
class Node:
    """One RESPONDER role bootloader to update with update_nodes()."""

    ids: ResponderIds
    image: bytes
    version: int
    force: bool = False
    device_type: int | None = None
    hardware_version: int | None = None


def update_nodes(
    bus: CommanderBus,
    nodes: list[Node],
    wait_timeout: float = 60.0,
    attempts: int = 3,
    log: Callable[[str], None] = print,
    progress: Callable[[int, int], None] | None = None,
) -> list[Outcome]:
    """Updates several RESPONDER role bootloaders on the bus, one after the other.

    The bootloaders respond only for a while after a reset, so first all of them are caught: their
    heartbeats are polled until each one has responded (the nodes found keep being polled, which
    keeps them in their bootloaders). Then they are updated in order, polling the heartbeats of the
    ones still waiting for their turn."""
    found: set[int] = set()
    deadline = bus.time() + wait_timeout
    while len(found) < len(nodes):
        for i, node in enumerate(nodes):
            data = bus.request(node.ids.status, p.FRAME_SIZE, 0.1)
            if data is not None and i not in found:
                found.add(i)
                log(f"Bootloader at STATUS 0x{node.ids.status:02X}: {p.Status.decode(data).describe()}")
        if len(found) < len(nodes) and bus.time() >= deadline:
            missing = [f"0x{n.ids.status:02X}" for i, n in enumerate(nodes) if i not in found]
            raise UpdateError(f"no STATUS response from {', '.join(missing)} within {wait_timeout} s")

    outcomes = []
    for i, node in enumerate(nodes):
        log(f"Updating the node at STATUS 0x{node.ids.status:02X}")
        updater = Updater(
            bus,
            node.ids,
            attempts=attempts,
            log=log,
            progress=progress,
            keep_alive=[n.ids.status for n in nodes[i + 1 :]],
        )
        outcomes.append(
            updater.update(
                node.image,
                node.version,
                force=node.force,
                wait_timeout=5.0,
                device_type=node.device_type,
                hardware_version=node.hardware_version,
            )
        )
    return outcomes
