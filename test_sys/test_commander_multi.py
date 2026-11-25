"""COMMANDER role with two providers on two adapters: which one is chosen, and falling back to the
other one when the chosen one fails."""

import time

import protocol as p
import pytest
from bus import CardResponderBus
from conftest import wait_until
from dut import Dut
from provider import Provider


@pytest.fixture(scope="module")
def firmware(build, swd):
    fw = build("commander")
    swd.flash(fw.bootloader, erase_all=True)
    return fw


class _Serving:
    """A provider on its own adapter, which can be made to disappear."""

    def __init__(self, card, image: bytes, version: int, index: int, log, **kwargs):
        self.card = card
        self.active = True
        self.provider = Provider(CardResponderBus(card), image, version, index=index, log=log, **kwargs)

    def disappear(self):
        self.card.cmd("C")
        self.active = False

    def poll(self):
        # Handle everything received so far without waiting: with a response queue, the card reports
        # every DATA frame it publishes, and the provider must keep up with them.
        while self.active:
            frame = self.provider.bus.receive(0)
            if frame is None:
                return
            self.provider.handle_frame(*frame)


def _serve_all(dut: Dut, servings: list[_Serving], timeout: float = 120) -> _Serving | None:
    """Runs the providers and resets the DUT; returns the one that reported a successful update."""
    for s in servings:
        s.provider.start()
    dut.reset()
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for s in servings:
            s.poll()
            if s.provider.result == p.Error.NONE:
                return s
        time.sleep(0.0005)
    return None


def _assert_installed(dut: Dut, image: bytes, version: int):
    installed = dut.installed()
    assert installed is not None and (installed.version, installed.size) == (version, len(image))
    assert dut.app_image(len(image)) == image
    assert wait_until(dut.app_running, 1.0)
    assert dut.led_toggles()


def test_highest_version_is_chosen(dut, card, card2, firmware, log):
    dut.install(firmware.blinky_slow, 0x0100)
    older = _Serving(card, firmware.blinky_slow, 0x0105, 0, log)
    newer = _Serving(card2, firmware.blinky_fast, 0x0107, 1, log)
    assert _serve_all(dut, [older, newer]) is newer
    assert older.provider.data is None and older.provider.result is None  # never selected
    _assert_installed(dut, firmware.blinky_fast, 0x0107)


def test_equal_versions_choose_the_lower_index(dut, card, card2, firmware, log):
    dut.install(firmware.blinky_slow, 0x0100)
    second = _Serving(card, firmware.blinky_slow, 0x0200, 3, log)
    first = _Serving(card2, firmware.blinky_fast, 0x0200, 0, log)
    assert _serve_all(dut, [second, first]) is first
    _assert_installed(dut, firmware.blinky_fast, 0x0200)


def test_force_wins_over_higher_version(dut, card, card2, firmware, log):
    dut.install(firmware.blinky_fast, 0x0100)
    newer = _Serving(card, firmware.blinky_fast, 0x0300, 0, log)
    forced = _Serving(card2, firmware.blinky_slow, 0x0100, 1, log, force=True)
    assert _serve_all(dut, [newer, forced]) is forced
    _assert_installed(dut, firmware.blinky_slow, 0x0100)


def test_falls_back_to_other_provider_when_chosen_disappears(dut, card, card2, firmware, big_image, log):
    dut.install(firmware.blinky_slow, 0x0100)
    fallback = _Serving(card, firmware.blinky_fast, 0x0105, 0, log)
    chosen = _Serving(card2, big_image, 0x0107, 1, log)

    def progress(done, _total):
        if done > 3000:
            chosen.disappear()

    chosen.provider.progress = progress
    assert _serve_all(dut, [fallback, chosen]) is fallback
    assert not chosen.active
    _assert_installed(dut, firmware.blinky_fast, 0x0105)


class _CorruptProvider(Provider):
    """Serves its image with a wrong CRC trailer: every transfer from it fails verification."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.stream = self.stream[:-4] + bytes(b ^ 0xFF for b in self.stream[-4:])


def test_falls_back_to_other_provider_when_chosen_is_corrupt(dut, card, card2, firmware, log):
    dut.install(firmware.blinky_slow, 0x0100)
    fallback = _Serving(card, firmware.blinky_fast, 0x0105, 0, log)
    corrupt = _Serving(card2, firmware.blinky_slow, 0x0107, 1, log)
    corrupt.provider = _CorruptProvider(
        CardResponderBus(card2), firmware.blinky_slow, 0x0107, index=1, log=log
    )
    assert _serve_all(dut, [fallback, corrupt], timeout=60) is fallback
    assert corrupt.provider.result == p.Error.CRC
    _assert_installed(dut, firmware.blinky_fast, 0x0105)
