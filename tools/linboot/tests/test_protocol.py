import protocol as p
import pytest


def test_versions():
    assert p.parse_version("1.2") == 0x0102
    assert p.parse_version("0x0304") == 0x0304
    assert p.parse_version("258") == 0x0102
    assert p.format_version(0x0A0B) == "10.11"
    for bad in ("256.0", "1.256", "0x10000", "-1"):
        with pytest.raises(ValueError):
            p.parse_version(bad)


def test_crc32_check_value():
    assert p.crc32(b"123456789") == 0xCBF43926


def test_offer_cmd():
    assert p.cmd_offer(0x0102, 0x123456, True) == bytes([0x01, 0x02, 0x01, 0x56, 0x34, 0x12, 0x01, 0x00])
    with pytest.raises(ValueError):
        p.cmd_offer(1, 0, False)


def test_finish_cmd():
    assert p.cmd_finish(0xCAFEBABE) == bytes([0x02, 0xBE, 0xBA, 0xFE, 0xCA, 0, 0, 0])


def test_chunk_frames():
    image = bytes(range(10))
    assert p.chunk_frame(image, 0) == bytes([0, 0, 1, 2, 3, 4, 5, 6])
    assert p.chunk_frame(image, 7) == bytes([1, 7, 8, 9, 0xFF, 0xFF, 0xFF, 0xFF])
    assert p.chunk_frame(bytes(7 * 300), 7 * 256)[0] == 0
    with pytest.raises(ValueError):
        p.chunk_frame(image, 3)


def test_status_decode():
    idle = p.Status.decode(bytes([0x01, 0x34, 0x12, 0x03, 0x00, 0x02, 0x01, 0x11]))
    assert (idle.state, idle.device_type, idle.hardware_version, idle.firmware_version) == (p.State.IDLE, 0x1234, 3, 0x0102)
    assert idle.app_valid and idle.protocol_version == 1
    rx = p.Status.decode(bytes([0x10, 0x04, 0x85, 0x00, 0x00, 0x00, 0x01, 0x00]))
    assert (rx.state, rx.error, rx.next_offset, rx.window_end) == (p.State.RECEIVING, p.Error.BAD_STATE, 133, 256)
    done = p.Status.decode(bytes([0x20, 0x00, 0x02, 0x01, 0, 0, 0, 0]))
    assert (done.state, done.firmware_version) == (p.State.DONE, 0x0102)
    for status in (idle, rx, done):
        assert status.describe()


def test_queries():
    code, identify = p.decode_query(bytes([0x01, 0x34, 0x12, 0x03, 0x00, 0x00, 0x00, 0x10]))
    assert code == p.Query.IDENTIFY and identify.device_type == 0x1234 and not identify.app_valid
    assert p.decode_query(bytes([0x02, 0x01, 0x07, 0x00, 0x00, 0, 0, 0])) == (p.Query.READ, (1, 7))
    assert p.decode_query(bytes([0x03, 0x02, 0x03, 0, 0, 0, 0, 0])) == (p.Query.RESULT, (2, p.Error.CRC))
    assert p.decode_query(bytes(8)) == (None, None)


def test_offer_and_stream():
    assert p.offer_frame(0x0102, 1000, False) == bytes([0x02, 0x01, 0xE8, 0x03, 0x00, 0x00, 0, 0])
    stream = p.provider_stream(b"abc")
    assert stream[:3] == b"abc" and int.from_bytes(stream[3:], "little") == p.crc32(b"abc")
