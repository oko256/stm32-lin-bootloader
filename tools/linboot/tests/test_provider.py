"""Provider logic against a scripted bus (no bootloader)."""

import protocol as p
from bus import ResponderBus
from provider import Provider


class FakeBus(ResponderBus):
    """Delivers scripted (time, id, data) frames, with a virtual clock."""

    def __init__(self, frames):
        self.frames = list(frames)
        self.now = 0.0
        self.configures = []

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds

    def configure(self, subscribe, publish):
        self.configures.append((dict(subscribe), dict(publish)))

    def receive(self, timeout):
        if self.frames and self.frames[0][0] <= self.now + timeout:
            t, lin_id, data = self.frames.pop(0)
            self.now = max(self.now, t)
            return lin_id, data
        self.now += timeout
        return None


IDENTIFY = bytes([0x01, 0x34, 0x12, 0x03, 0x00, 0x00, 0x01, 0x11])


def _read(offset):
    return bytes([0x02, 0x00]) + offset.to_bytes(3, "little") + bytes(3)


def _quiet(_text):
    pass


def test_repeated_read_does_not_rewrite_the_table():
    image = bytes(range(20))
    bus = FakeBus([(0.0, 0x31, IDENTIFY), (0.1, 0x31, _read(0)), (0.2, 0x31, _read(0)), (0.3, 0x31, _read(7))])
    Provider(bus, image, 0x0200, log=_quiet).run(timeout=1.0)
    # Start, the offer after IDENTIFY, READ 0 and READ 7: the repeated READ 0 doesn't rewrite.
    assert len(bus.configures) == 4
    assert bus.configures[1][1] == {0x33: p.offer_frame(0x0200, 20, False)}
    assert bus.configures[-1][1][0x32] == p.chunk_frame(p.provider_stream(image), 7)


def test_missed_result_is_assumed_done_after_quiet_period():
    image = bytes(range(10))  # a stream of 14 bytes: chunks at 0 and 7
    bus = FakeBus([(0.0, 0x31, IDENTIFY), (0.1, 0x31, _read(0)), (0.2, 0x31, _read(7))])
    assert Provider(bus, image, 0x0200, log=_quiet).run(timeout=10.0, quiet_timeout=2.0) == p.Error.NONE
    assert 2.0 <= bus.now < 3.0


def test_quiet_without_full_read_keeps_waiting():
    bus = FakeBus([(0.0, 0x31, IDENTIFY), (0.1, 0x31, _read(0))])
    assert Provider(bus, bytes(range(100)), 0x0200, log=_quiet).run(timeout=5.0) is None


def test_stops_serving_data_when_another_provider_is_selected():
    image = bytes(range(20))
    other_read = bytes([0x02, 0x01, 0, 0, 0, 0, 0, 0])
    bus = FakeBus([(0.0, 0x31, IDENTIFY), (0.1, 0x31, _read(0)), (0.2, 0x31, other_read)])
    Provider(bus, image, 0x0200, log=_quiet).run(timeout=1.0)
    assert 0x32 in bus.configures[-2][1]
    assert bus.configures[-1][1] == {0x33: p.offer_frame(0x0200, 20, False)}


def test_stops_serving_data_on_new_discovery():
    image = bytes(range(20))
    bus = FakeBus([(0.0, 0x31, IDENTIFY), (0.1, 0x31, _read(0)), (0.2, 0x31, IDENTIFY)])
    Provider(bus, image, 0x0200, log=_quiet).run(timeout=1.0)
    assert bus.configures[-1][1] == {0x33: p.offer_frame(0x0200, 20, False)}
