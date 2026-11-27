"""COMMANDER role: the DUT is the bus commander and pulls its update from the adapter, which acts
as one (or several) providers (tools/linboot/provider.py)."""

import threading
import time

import protocol as p
import pytest
from bus import CardResponderBus, ResponderBus
from conftest import run_tool, wait_until
from dut import DEVICE_TYPE, HW_VERSION, Dut
from provider import Provider

QUERY_ID = 0x31
OFFER_IDS = [0x33, 0x34, 0x35, 0x36]


class Abort(Exception):
    pass


@pytest.fixture(scope="module")
def firmware(build, swd):
    fw = build("commander")
    swd.flash(fw.bootloader, erase_all=True)
    return fw


@pytest.fixture
def provider(card, log):
    """Returns a function that makes a provider on the adapter."""
    bus = CardResponderBus(card)

    def make(image: bytes, version: int, **kwargs) -> Provider:
        return Provider(bus, image, version, log=log, **kwargs)

    return make


def _assert_installed(dut: Dut, image: bytes, version: int):
    installed = dut.installed()
    assert installed is not None and (installed.version, installed.size) == (version, len(image))
    assert dut.app_image(len(image)) == image
    assert wait_until(dut.app_running, 1.0)
    assert dut.led_toggles()


def _serve(dut: Dut, provider: Provider, timeout: float = 60, reset: bool = True):
    """Runs the provider; resets the DUT once the provider is ready (if reset)."""
    if reset:
        provider.start()
        dut.reset()
    return provider.run(timeout=timeout)


def test_looks_for_updates_without_application(dut, card, firmware):
    dut.erase_app()
    dut.reset()
    card.open_monitor()
    time.sleep(3)
    frames = card.frames()
    identifies = [f for f in frames if f.id == QUERY_ID]
    assert len(identifies) > 20  # keeps asking, also after the boot timeout
    code, identify = p.decode_query(identifies[-1].data)
    assert code == p.Query.IDENTIFY
    assert (identify.device_type, identify.hardware_version) == (DEVICE_TYPE, HW_VERSION)
    assert not identify.app_valid and identify.firmware_version == 0
    # All the providers' OFFER headers are polled (no responses: reported as headers only).
    assert {f.id for f in frames if not f.data} >= set(OFFER_IDS)
    assert not dut.app_running()


def test_first_install(dut, provider, firmware):
    dut.erase_app()
    dut.reset()
    assert _serve(dut, provider(firmware.blinky_slow, 0x0100), reset=False) == p.Error.NONE
    _assert_installed(dut, firmware.blinky_slow, 0x0100)


def test_update_after_reset(dut, provider, firmware):
    dut.install(firmware.blinky_slow, 0x0100)
    serving = provider(firmware.blinky_fast, 0x0101, device_type=DEVICE_TYPE, hardware_version=HW_VERSION)
    assert _serve(dut, serving) == p.Error.NONE
    assert serving.last_identify.firmware_version == 0x0100
    _assert_installed(dut, firmware.blinky_fast, 0x0101)


def test_same_version_boots_right_away(dut, provider, firmware):
    dut.install(firmware.blinky_fast, 0x0101)
    serving = provider(firmware.blinky_slow, 0x0101)
    thread = threading.Thread(target=serving.run, kwargs={"timeout": 3})
    serving.start()
    thread.start()
    dut.reset()
    start = time.monotonic()
    assert wait_until(dut.app_running, 1.0)  # well before the 2 s boot timeout
    print(f"application running {time.monotonic() - start:.2f} s after the reset")
    thread.join()
    assert serving.result is None  # no transfer
    _assert_installed(dut, firmware.blinky_fast, 0x0101)


def test_force_reinstalls_same_version(dut, provider, firmware):
    dut.install(firmware.blinky_slow, 0x0101)
    assert _serve(dut, provider(firmware.blinky_fast, 0x0101, force=True)) == p.Error.NONE
    _assert_installed(dut, firmware.blinky_fast, 0x0101)


def test_other_device_type_is_not_offered(dut, provider, firmware):
    dut.install(firmware.blinky_slow, 0x0100)
    serving = provider(firmware.blinky_fast, 0x0200, device_type=0x9999)
    assert _serve(dut, serving, timeout=4) is None
    _assert_installed(dut, firmware.blinky_slow, 0x0100)


def test_boots_after_timeout_without_provider(dut, firmware):
    dut.install(firmware.blinky_slow, 0x0100)
    dut.reset()
    time.sleep(1.0)
    assert not dut.app_running()
    assert wait_until(dut.app_running, 2.0)
    assert dut.led_toggles()


class _SharedCard(ResponderBus):
    """Several providers on one card: their responder tables are merged."""

    def __init__(self, bus: CardResponderBus):
        self.bus = bus
        self.tables: dict[int, tuple[dict, dict]] = {}

    def view(self, key: int) -> ResponderBus:
        shared = self

        class View(ResponderBus):
            def configure(self, subscribe, publish):
                shared.tables[key] = (subscribe, publish)
                merged_sub, merged_pub = {}, {}
                for sub, pub in shared.tables.values():
                    merged_sub.update(sub)
                    merged_pub.update(pub)
                shared.bus.configure(merged_sub, merged_pub)

            def receive(self, timeout):
                raise NotImplementedError

        return View()

    def configure(self, subscribe, publish):
        raise NotImplementedError

    def receive(self, timeout):
        return self.bus.receive(timeout)


def test_highest_offered_version_is_chosen(dut, card, firmware, log):
    dut.install(firmware.blinky_slow, 0x0100)
    shared = _SharedCard(CardResponderBus(card))
    older = Provider(shared.view(0), firmware.blinky_slow, 0x0105, index=0, log=log)
    newer = Provider(shared.view(2), firmware.blinky_fast, 0x0107, index=2, log=log)
    older.start()
    newer.start()
    dut.reset()
    end = time.monotonic() + 60
    while newer.result is None and time.monotonic() < end:
        frame = shared.receive(0.1)
        if frame is not None:
            older.handle_frame(*frame)
            newer.handle_frame(*frame)
    assert newer.result == p.Error.NONE
    assert older.data is None  # never got a READ
    _assert_installed(dut, firmware.blinky_fast, 0x0107)


def test_interrupted_transfer_recovers(dut, card, provider, firmware, big_image):
    dut.install(firmware.blinky_slow, 0x0100)

    def progress(done, _total):
        if done > 3000:
            raise Abort()

    with pytest.raises(Abort):
        _serve(dut, provider(big_image, 0x0200, progress=progress))
    card.wipe()  # the provider disappears

    # The old application is gone: the bootloader keeps looking, also after a reset.
    time.sleep(3)
    assert not dut.app_running()
    assert dut.installed() is None
    dut.reset()
    time.sleep(3)
    assert not dut.app_running()

    assert _serve(dut, provider(firmware.blinky_slow, 0x0100), reset=False) == p.Error.NONE
    _assert_installed(dut, firmware.blinky_slow, 0x0100)


@pytest.mark.slow
def test_large_image(dut, provider, firmware, big_image):
    dut.install(firmware.blinky_slow, 0x0100)
    serving = provider(big_image, 0x0200)
    start = time.monotonic()
    assert _serve(dut, serving, timeout=180) == p.Error.NONE
    elapsed = time.monotonic() - start
    print(f"{len(big_image)} bytes in {elapsed:.1f} s ({'queued' if serving.streaming else 'no'} responses)")
    _assert_installed(dut, big_image, 0x0200)
    if serving.streaming:
        # The DATA frames are polled back to back (about 60 s without the response queue).
        assert elapsed < 35


def test_cli_serve(dut, firmware, adapter_port, tmp_path):
    dut.install(firmware.blinky_slow, 0x0100)
    image = tmp_path / "app.bin"
    image.write_bytes(firmware.blinky_fast)
    result = run_tool(adapter_port, "serve", str(image), "--version", "2.1", "--timeout", "60", reset_dut=dut)
    assert result.returncode == 0 and "Update done" in result.stdout
    _assert_installed(dut, firmware.blinky_fast, 0x0201)
