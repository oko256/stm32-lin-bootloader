"""RESPONDER role: the adapter is the bus commander and updates the DUT (tools/linboot/updater.py)."""

import time

import protocol as p
import pytest
from bus import CardCommanderBus
from conftest import reset_soon, run_tool, wait_until
from dut import DEVICE_TYPE, HW_VERSION, Dut
from updater import Outcome, UpdateError, Updater


class Abort(Exception):
    pass


@pytest.fixture(scope="module")
def firmware(build, swd):
    fw = build("responder")
    swd.flash(fw.bootloader, erase_all=True)
    return fw


@pytest.fixture
def updater(card, log):
    return Updater(CardCommanderBus(card), log=log)


def _assert_installed(dut: Dut, image: bytes, version: int):
    installed = dut.installed()
    assert installed is not None and (installed.version, installed.size) == (version, len(image))
    assert dut.app_image(len(image)) == image
    assert wait_until(dut.app_running, 1.0)
    assert dut.led_toggles()


def test_status_without_application(dut, updater, firmware):
    dut.erase_app()
    dut.reset()
    status = updater.read_status(2.0)
    assert status is not None
    assert status.state == p.State.IDLE
    assert (status.device_type, status.hardware_version) == (DEVICE_TYPE, HW_VERSION)
    assert not status.app_valid and status.firmware_version == 0
    assert status.protocol_version == p.PROTOCOL_VERSION
    # Without an application, the bootloader keeps waiting.
    time.sleep(3)
    assert not dut.app_running()
    assert updater.read_status(1.0) is not None


def test_first_install(dut, updater, firmware):
    dut.erase_app()
    dut.reset()
    assert updater.update(firmware.blinky_slow, 0x0100, device_type=DEVICE_TYPE, hardware_version=HW_VERSION) == Outcome.UPDATED
    _assert_installed(dut, firmware.blinky_slow, 0x0100)


def test_update_in_boot_window(dut, updater, firmware):
    dut.install(firmware.blinky_slow, 0x0100)
    dut.reset()
    assert wait_until(dut.app_running, 3.0)
    reset_soon(dut)
    assert updater.update(firmware.blinky_fast, 0x0101, wait_timeout=5) == Outcome.UPDATED
    _assert_installed(dut, firmware.blinky_fast, 0x0101)


def test_same_version_is_up_to_date(dut, updater, firmware, log):
    dut.install(firmware.blinky_fast, 0x0101)
    reset_soon(dut)
    assert updater.update(firmware.blinky_slow, 0x0101, wait_timeout=5) == Outcome.UP_TO_DATE
    # BOOT was sent: the application starts right away, untouched.
    assert wait_until(dut.app_running, 0.5)
    _assert_installed(dut, firmware.blinky_fast, 0x0101)


def test_force_reinstalls_same_version(dut, updater, firmware):
    dut.install(firmware.blinky_slow, 0x0101)
    reset_soon(dut)
    assert updater.update(firmware.blinky_fast, 0x0101, force=True, wait_timeout=5) == Outcome.UPDATED
    _assert_installed(dut, firmware.blinky_fast, 0x0101)


def test_older_version_is_installed(dut, updater, firmware):
    dut.install(firmware.blinky_fast, 0x0200)
    reset_soon(dut)
    assert updater.update(firmware.blinky_slow, 0x0105, wait_timeout=5) == Outcome.UPDATED
    _assert_installed(dut, firmware.blinky_slow, 0x0105)


def test_boots_after_timeout_without_updater(dut, firmware):
    dut.install(firmware.blinky_slow, 0x0100)
    dut.reset()
    time.sleep(1.0)
    assert not dut.app_running()
    assert wait_until(dut.app_running, 2.0)
    assert dut.led_toggles()


def test_wrong_device_type_is_not_updated(dut, updater, firmware):
    dut.install(firmware.blinky_slow, 0x0100)
    reset_soon(dut)
    with pytest.raises(UpdateError, match="device type"):
        updater.update(firmware.blinky_fast, 0x0200, device_type=0x9999, wait_timeout=5)
    updater.boot()
    _assert_installed(dut, firmware.blinky_slow, 0x0100)


def test_interrupted_transfer_recovers(dut, updater, firmware, big_image, card):
    dut.install(firmware.blinky_slow, 0x0100)

    def progress(done, _total):
        if done > 3000:
            raise Abort()

    updater.progress = progress
    reset_soon(dut)
    with pytest.raises(Abort):
        updater.update(big_image, 0x0200, wait_timeout=5)
    updater.progress = None

    # The old application is gone, so the bootloader stays, also after a reset.
    dut.reset()
    time.sleep(3)
    assert not dut.app_running()
    assert dut.installed() is None
    status = updater.read_status(1.0)
    assert status is not None and status.state == p.State.IDLE and not status.app_valid

    assert updater.update(firmware.blinky_slow, 0x0100) == Outcome.UPDATED
    _assert_installed(dut, firmware.blinky_slow, 0x0100)


@pytest.mark.slow
def test_large_image(dut, updater, firmware, big_image):
    dut.install(firmware.blinky_slow, 0x0100)
    reset_soon(dut)
    start = time.monotonic()
    assert updater.update(big_image, 0x0200, wait_timeout=5) == Outcome.UPDATED
    print(f"{len(big_image)} bytes in {time.monotonic() - start:.1f} s")
    _assert_installed(dut, big_image, 0x0200)


def test_cli_update(dut, firmware, adapter_port, tmp_path):
    dut.install(firmware.blinky_slow, 0x0100)
    image = tmp_path / "app.bin"
    image.write_bytes(firmware.blinky_fast)
    result = run_tool(adapter_port, "update", str(image), "--version", "2.1", reset_dut=dut)
    assert result.returncode == 0 and "Result: updated" in result.stdout
    _assert_installed(dut, firmware.blinky_fast, 0x0201)

    result = run_tool(adapter_port, "update", str(image), "--version", "2.1", reset_dut=dut)
    assert result.returncode == 0 and "Result: up to date" in result.stdout
