"""End-to-end tests of the host tool against the simulated bootloader (real C state machines)."""

import random

import protocol as p
import pytest
from provider import CommanderIds, Provider
from updater import Outcome, UpdateError, Updater
from sim import DEVICE_TYPE, HW_VERSION, OFFER_IDS, SimCommanderBus, SimResponderBus, load_lib

LIB = load_lib()
pytestmark = pytest.mark.skipif(LIB is None, reason="simulated bootloader not built (build the unit tests first)")

APP_MAX = 2048


def _image(size: int, seed: int = 0) -> bytes:
    return random.Random(seed).randbytes(size)


def _quiet(_text: str) -> None:
    pass


# RESPONDER role: the host updates the bootloader.


def test_update_responder():
    bus = SimResponderBus(LIB, app_size=500, version=0x0100)
    image = _image(1500)
    updater = Updater(bus, log=_quiet)
    assert updater.update(image, 0x0102, device_type=DEVICE_TYPE, hardware_version=HW_VERSION) == Outcome.UPDATED
    assert bus.installed() == (image, 0x0102)
    assert bus.booted


def test_update_responder_full_size_with_losses():
    bus = SimResponderBus(LIB, loss=0.1, seed=3)
    image = _image(APP_MAX, seed=1)
    assert Updater(bus, log=_quiet).update(image, 0x0001) == Outcome.UPDATED
    assert bus.installed() == (image, 0x0001)


def test_update_responder_same_version():
    bus = SimResponderBus(LIB, app_size=500, version=0x0100)
    updater = Updater(bus, log=_quiet)
    assert updater.update(_image(700), 0x0100) == Outcome.UP_TO_DATE
    assert bus.booted
    assert bus.installed()[1] == 0x0100

    bus = SimResponderBus(LIB, app_size=500, version=0x0100)
    image = _image(700)
    assert Updater(bus, log=_quiet).update(image, 0x0100, force=True) == Outcome.UPDATED
    assert bus.installed() == (image, 0x0100)


def test_update_responder_wrong_device():
    bus = SimResponderBus(LIB)
    with pytest.raises(UpdateError, match="device type"):
        Updater(bus, log=_quiet).update(_image(100), 1, device_type=0x9999)
    with pytest.raises(UpdateError, match="hardware version"):
        Updater(bus, log=_quiet).update(_image(100), 1, hardware_version=0x9999)


def test_update_responder_too_big():
    bus = SimResponderBus(LIB)
    with pytest.raises(UpdateError, match="doesn't fit"):
        Updater(bus, log=_quiet).update(_image(APP_MAX + 1), 1)


def test_responder_boots_without_heartbeat():
    bus = SimResponderBus(LIB, app_size=500, version=0x0100)
    bus.sleep(2.5)
    assert bus.booted
    with pytest.raises(UpdateError, match="no STATUS"):
        Updater(bus, log=_quiet).update(_image(100), 2, wait_timeout=1.0)


# COMMANDER role: the host provides the update.


def _provider(bus, image, version, **kwargs):
    return Provider(bus, image, version, ids=CommanderIds(offers=OFFER_IDS), log=_quiet, **kwargs)


# Response queue depth and USB latency of the bus: without a queue (stock USB LIN Card firmware),
# and with one (firmware 6).
BUS_MODES = {"no-queue": {}, "queue": {"queue_depth": 16, "latency_ms": 3}}


@pytest.fixture(params=BUS_MODES.values(), ids=BUS_MODES.keys())
def bus_mode(request):
    return request.param


def test_serve_commander(bus_mode):
    bus = SimCommanderBus(LIB, app_size=500, version=0x0100, **bus_mode)
    image = _image(1500)
    provider = _provider(bus, image, 0x0102, device_type=DEVICE_TYPE, hardware_version=HW_VERSION)
    assert provider.run(timeout=120) == p.Error.NONE
    bus.run_for(0.1)
    assert bus.booted
    assert bus.installed() == (image, 0x0102)
    assert provider.last_identify.firmware_version == 0x0100


def test_serve_commander_with_losses_as_second_provider(bus_mode):
    bus = SimCommanderBus(LIB, loss=0.1, seed=5, **bus_mode)
    image = _image(APP_MAX, seed=2)
    provider = _provider(bus, image, 0x0203, index=2)
    assert provider.run(timeout=300) == p.Error.NONE
    bus.run_for(0.1)
    assert bus.installed() == (image, 0x0203)


def _serve_time(image, **bus_mode) -> tuple[float, int]:
    """Serves @p image to a bootloader without an application; returns the (virtual) time it took
    and the number of READs."""
    bus = SimCommanderBus(LIB, **bus_mode)
    start = bus.time()
    assert _provider(bus, image, 0x0100).run(timeout=300) == p.Error.NONE
    bus.run_for(0.1)
    assert bus.installed() == (image, 0x0100)
    reads = sum(1 for query in bus.queries if query[0] == p.Query.READ)
    return bus.time() - start, reads


def test_serve_commander_queue_streams_data():
    image = _image(APP_MAX)
    plain_time, plain_reads = _serve_time(image)
    queue_time, queue_reads = _serve_time(image, **BUS_MODES["queue"])
    # Without a queue, nearly every chunk needs a READ; with one, only the first does.
    assert plain_reads > len(image) // p.CHUNK_SIZE
    assert queue_reads <= 2
    assert queue_time < plain_time / 2


def test_serve_commander_queue_runs_empty():
    # The host is slower than the bus: the queue runs empty and the bootloader gets stale chunks,
    # which it resynchronizes with READs.
    image = _image(APP_MAX, seed=3)
    _, reads = _serve_time(image, queue_depth=1, latency_ms=25)
    assert reads > 10


def test_serve_commander_up_to_date():
    bus = SimCommanderBus(LIB, app_size=500, version=0x0100)
    provider = _provider(bus, _image(700), 0x0100)
    assert provider.run(timeout=5) is None
    assert bus.booted
    assert OFFER_IDS[0] in bus.headers and 0x32 not in bus.headers


def test_serve_commander_force():
    bus = SimCommanderBus(LIB, app_size=500, version=0x0100)
    image = _image(700)
    assert _provider(bus, image, 0x0100, force=True).run(timeout=120) == p.Error.NONE
    bus.run_for(0.1)
    assert bus.installed() == (image, 0x0100)


def test_serve_commander_wrong_device():
    bus = SimCommanderBus(LIB, app_size=500, version=0x0100)
    provider = _provider(bus, _image(700), 0x0200, device_type=0x9999)
    assert provider.run(timeout=5) is None
    assert bus.booted  # after the boot timeout
    assert bus.installed()[1] == 0x0100
