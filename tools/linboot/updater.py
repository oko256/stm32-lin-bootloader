"""Updater: the host is the bus commander and updates a RESPONDER role bootloader."""

from dataclasses import dataclass
from enum import Enum
from typing import Callable

import protocol as p
from bus import CommanderBus


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
    """Runs the update sequence: wait for the heartbeat response, OFFER, stream the chunks up to the
    window the bootloader reports, FINISH, and BOOT."""

    def __init__(
        self,
        bus: CommanderBus,
        ids: ResponderIds = ResponderIds(),
        attempts: int = 3,
        status_timeout: float = 0.1,
        stall_timeout: float = 3.0,
        log: Callable[[str], None] = print,
        progress: Callable[[int, int], None] | None = None,
    ):
        self.bus = bus
        self.ids = ids
        self.attempts = attempts
        self.status_timeout = status_timeout
        self.stall_timeout = stall_timeout
        self.log = log
        self.progress = progress

    def read_status(self, timeout: float | None = None) -> p.Status | None:
        data = self.bus.request(self.ids.status, p.FRAME_SIZE, self.status_timeout if timeout is None else timeout)
        return None if data is None else p.Status.decode(data)

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

        # Stream the chunks; the bootloader reports where it is and how far we may go.
        last_progress = self.bus.time()
        last_next = -1
        while True:
            if status.state != p.State.RECEIVING:
                self.log(f"Transfer interrupted: {status.describe()}")
                return None
            next_offset = status.next_offset
            if next_offset != last_next:
                last_next = next_offset
                last_progress = self.bus.time()
                if self.progress is not None:
                    self.progress(min(next_offset, size), size)
            elif self.bus.time() - last_progress > self.stall_timeout:
                self.log("Transfer stalled")
                return None
            if next_offset >= size:
                break
            for offset in range(next_offset, min(status.window_end, size), p.CHUNK_SIZE):
                self.bus.send_frame(self.ids.data, p.chunk_frame(image, offset))
            status = self._status_until(self.stall_timeout)

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
            if status.state == p.State.RECEIVING and status.error == p.Error.BAD_STATE:
                # FINISH came before the last chunk got in: some chunks were lost after all.
                self.log("FINISH was too early, retrying")
                return None
            if status.state == p.State.RECEIVING:
                # FINISH was lost on the bus.
                self.send_cmd(p.cmd_finish(p.crc32(image)))
