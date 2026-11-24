"""Bootloader LIN protocol: frame layouts and codes. Mirrors src/core/proto.h.

All bootloader frames are 8 bytes, multi-byte fields are little-endian. Image data is carried in
chunks of 7 bytes: a DATA frame is [seq][7 image bytes], where chunk k covers image offset 7*k and
seq is the low byte of k.
"""

import zlib
from dataclasses import dataclass
from enum import IntEnum

PROTOCOL_VERSION = 1
FRAME_SIZE = 8
CHUNK_SIZE = 7
TRAILER_SIZE = 4
MAX_IMAGE_SIZE = 0xFFFFFF

OFFER_FLAG_FORCE = 0x01
STATUS_FLAG_APP_VALID = 0x01


class State(IntEnum):
    """RESPONDER role: state byte of the STATUS frame."""

    IDLE = 0x01
    REJECTED = 0x02
    RECEIVING = 0x10
    VERIFYING = 0x11
    DONE = 0x20
    FAILED = 0x21


class Cmd(IntEnum):
    """RESPONDER role: command code of the CMD frame."""

    OFFER = 0x01
    FINISH = 0x02
    ABORT = 0x03
    BOOT = 0x04


class Query(IntEnum):
    """COMMANDER role: request code of the QUERY frame."""

    IDENTIFY = 0x01
    READ = 0x02
    RESULT = 0x03


class Error(IntEnum):
    NONE = 0x00
    BAD_SIZE = 0x01
    FLASH = 0x02
    CRC = 0x03
    BAD_STATE = 0x04
    TIMEOUT = 0x05


def error_name(code: int) -> str:
    try:
        return Error(code).name
    except ValueError:
        return f"0x{code:02X}"


def crc32(data: bytes) -> int:
    """CRC-32/ISO-HDLC, the same as the bootloader's."""
    return zlib.crc32(data) & 0xFFFFFFFF


def parse_version(text: str) -> int:
    """Parses "major.minor" (decimal) or a plain integer ("0x0102", "258") into a u16 version."""
    if "." in text:
        major, minor = text.split(".", 1)
        value = (int(major) << 8) | int(minor)
        if not (0 <= int(major) <= 255 and 0 <= int(minor) <= 255):
            raise ValueError(f"version {text!r} out of range")
    else:
        value = int(text, 0)
    if not 0 <= value <= 0xFFFF:
        raise ValueError(f"version {text!r} out of range")
    return value


def format_version(version: int) -> str:
    return f"{version >> 8}.{version & 0xFF}"


def _u16(data: bytes, pos: int) -> int:
    return int.from_bytes(data[pos : pos + 2], "little")


def _u24(data: bytes, pos: int) -> int:
    return int.from_bytes(data[pos : pos + 3], "little")


@dataclass
class Status:
    """A decoded STATUS frame (RESPONDER role)."""

    state: int
    error: int = 0
    device_type: int = 0
    hardware_version: int = 0
    firmware_version: int = 0
    app_valid: bool = False
    protocol_version: int = 0
    next_offset: int = 0
    window_end: int = 0

    @staticmethod
    def decode(data: bytes) -> "Status":
        if len(data) != FRAME_SIZE:
            raise ValueError(f"STATUS must be {FRAME_SIZE} bytes, got {len(data)}")
        state = data[0]
        if state in (State.IDLE, State.REJECTED):
            return Status(
                state=state,
                device_type=_u16(data, 1),
                hardware_version=_u16(data, 3),
                firmware_version=_u16(data, 5),
                app_valid=bool(data[7] & STATUS_FLAG_APP_VALID),
                protocol_version=data[7] >> 4,
            )
        if state in (State.RECEIVING, State.VERIFYING):
            return Status(state=state, error=data[1], next_offset=_u24(data, 2), window_end=_u24(data, 5))
        return Status(state=state, error=data[1], firmware_version=_u16(data, 2))

    def describe(self) -> str:
        try:
            name = State(self.state).name
        except ValueError:
            name = f"0x{self.state:02X}"
        if self.state in (State.IDLE, State.REJECTED):
            return (
                f"{name}: device type 0x{self.device_type:04X}, hardware 0x{self.hardware_version:04X}, "
                f"firmware {format_version(self.firmware_version)}"
                f"{'' if self.app_valid else ' (no valid application)'}, protocol {self.protocol_version}"
            )
        if self.state in (State.RECEIVING, State.VERIFYING):
            return f"{name}: next offset {self.next_offset}, window end {self.window_end}, error {error_name(self.error)}"
        return f"{name}: error {error_name(self.error)}, firmware {format_version(self.firmware_version)}"


def cmd_offer(version: int, size: int, force: bool) -> bytes:
    if not 0 < size <= MAX_IMAGE_SIZE:
        raise ValueError(f"image size {size} out of range")
    flags = OFFER_FLAG_FORCE if force else 0
    return bytes([Cmd.OFFER]) + version.to_bytes(2, "little") + size.to_bytes(3, "little") + bytes([flags, 0])


def cmd_finish(crc: int) -> bytes:
    return bytes([Cmd.FINISH]) + crc.to_bytes(4, "little") + bytes(3)


def cmd_simple(cmd: Cmd) -> bytes:
    return bytes([cmd]) + bytes(7)


def chunk_frame(stream: bytes, offset: int) -> bytes:
    """DATA frame for the chunk at @p offset (a multiple of CHUNK_SIZE) of @p stream, padded with
    0xFF past its end."""
    if offset % CHUNK_SIZE != 0:
        raise ValueError(f"offset {offset} is not a chunk boundary")
    data = stream[offset : offset + CHUNK_SIZE]
    data += b"\xff" * (CHUNK_SIZE - len(data))
    return bytes([(offset // CHUNK_SIZE) & 0xFF]) + data


@dataclass
class Identify:
    """A decoded IDENTIFY query (COMMANDER role)."""

    device_type: int
    hardware_version: int
    firmware_version: int
    app_valid: bool
    protocol_version: int


def decode_query(data: bytes):
    """Decodes a QUERY frame into (Query, details): Identify, (provider, offset) or (provider, error)."""
    if len(data) != FRAME_SIZE:
        raise ValueError(f"QUERY must be {FRAME_SIZE} bytes, got {len(data)}")
    code = data[0]
    if code == Query.IDENTIFY:
        return Query.IDENTIFY, Identify(
            device_type=_u16(data, 1),
            hardware_version=_u16(data, 3),
            firmware_version=_u16(data, 5),
            app_valid=bool(data[7] & STATUS_FLAG_APP_VALID),
            protocol_version=data[7] >> 4,
        )
    if code == Query.READ:
        return Query.READ, (data[1], _u24(data, 2))
    if code == Query.RESULT:
        return Query.RESULT, (data[1], data[2])
    return None, None


def offer_frame(version: int, size: int, force: bool) -> bytes:
    """OFFER response of a provider (COMMANDER role)."""
    if not 0 < size <= MAX_IMAGE_SIZE:
        raise ValueError(f"image size {size} out of range")
    flags = OFFER_FLAG_FORCE if force else 0
    return version.to_bytes(2, "little") + size.to_bytes(3, "little") + bytes([flags, 0, 0])


def provider_stream(image: bytes) -> bytes:
    """Data stream a provider serves: the image followed by its CRC32 (little-endian)."""
    return image + crc32(image).to_bytes(4, "little")
