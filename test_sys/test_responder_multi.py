"""RESPONDER role with two DUTs on the bus, updated by the adapter as the commander
(tools/linboot/updater.py update_nodes()).

The second DUT has no SWD: it's a USB LIN Card flashed (over USB DFU) with the bootloader of the
usb-lin-card-responder-node2-release preset (IDs 0x20..0x22), see README.md (System tests). It must
always be left with blinky-reboot (or nothing) installed: it resets itself every few seconds, which
is the only way it gets back to its bootloader. What it has installed is seen from its STATUS.
"""

import random
import threading
import time

import protocol as p
import pytest
from bus import CardCommanderBus
from conftest import reset_soon, run_tool, wait_until
from dut import Dut
from updater import Node, Outcome, ResponderIds, Updater, update_nodes
from usblincard import UsbLinCard

IDS1 = ResponderIds(0x30, 0x31, 0x32)
IDS2 = ResponderIds(0x20, 0x21, 0x22)

# blinky-reboot: bootloader window (2 s) + application (3 s) per cycle.
DUT2_CYCLE_S = 5.0


@pytest.fixture(scope="module")
def firmware(build, swd):
    fw = build("responder")
    swd.flash(fw.bootloader, erase_all=True)
    return fw


@pytest.fixture(scope="module")
def blinky_reboot(build) -> bytes:
    out = build("responder").bootloader.parent
    return (out / "examples" / "blinky" / "blinky-reboot.bin").read_bytes()


@pytest.fixture
def bus(card):
    return CardCommanderBus(card)


def _dut2_status(bus, timeout: float) -> p.Status | None:
    """Waits for the second DUT to come to its bootloader; returns its (IDLE) STATUS and lets it
    go on (BOOT)."""
    updater = Updater(bus, IDS2, log=lambda _: None)
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        status = updater.read_status(0.3)
        if status is not None:
            updater.boot()
            return status
    return None


@pytest.fixture
def dut2_version(bus) -> int:
    """The firmware version on the second DUT (the test is skipped if there's no second DUT)."""
    status = _dut2_status(bus, 2 * DUT2_CYCLE_S)
    if status is None:
        pytest.skip("no second RESPONDER DUT (STATUS 0x20) on the bus")
    return status.firmware_version


def _next_version(version: int) -> int:
    """A different version (the minor number cycles), so that the update is taken."""
    return (version & 0xFF00) | ((version + 1) & 0xFF)


def _assert_dut1(dut: Dut, image: bytes, version: int):
    installed = dut.installed()
    assert installed is not None and (installed.version, installed.size) == (version, len(image))
    assert dut.app_image(len(image)) == image
    assert wait_until(dut.app_running, 1.0)
    assert dut.led_toggles()


def _assert_dut2(bus, version: int):
    # It comes back to its bootloader (after its application reset itself) with the new version.
    status = _dut2_status(bus, 2 * DUT2_CYCLE_S)
    assert status is not None
    assert status.app_valid and status.firmware_version == version


def test_update_both_nodes(dut, bus, firmware, blinky_reboot, dut2_version, log):
    dut.install(firmware.blinky_slow, 0x0100)
    version2 = _next_version(dut2_version)
    reset_soon(dut)
    outcomes = update_nodes(
        bus,
        [Node(IDS1, firmware.blinky_fast, 0x0101), Node(IDS2, blinky_reboot, version2)],
        wait_timeout=2 * DUT2_CYCLE_S,
        log=log,
    )
    assert outcomes == [Outcome.UPDATED, Outcome.UPDATED]
    _assert_dut1(dut, firmware.blinky_fast, 0x0101)
    _assert_dut2(bus, version2)


def test_one_node_up_to_date(dut, bus, firmware, blinky_reboot, dut2_version, log):
    dut.install(firmware.blinky_fast, 0x0101)
    version2 = _next_version(dut2_version)
    reset_soon(dut)
    outcomes = update_nodes(
        bus,
        [Node(IDS2, blinky_reboot, version2), Node(IDS1, firmware.blinky_slow, 0x0101)],
        wait_timeout=2 * DUT2_CYCLE_S,
        log=log,
    )
    assert outcomes == [Outcome.UPDATED, Outcome.UP_TO_DATE]
    _assert_dut1(dut, firmware.blinky_fast, 0x0101)
    _assert_dut2(bus, version2)


@pytest.mark.slow
def test_waiting_node_is_kept_in_its_bootloader(dut, bus, firmware, blinky_reboot, dut2_version, log):
    # The second DUT gets a 20 KB image (~40 s), much longer than the session timeout (5 s). The
    # first DUT has a valid application and waits for its turn: it must stay in its bootloader
    # (checked over SWD) instead of starting its application.
    big_reboot = blinky_reboot + random.Random(2).randbytes(20 * 1024 - len(blinky_reboot))
    dut.install(firmware.blinky_slow, 0x0100)
    version2 = _next_version(dut2_version)

    dut1_turn = threading.Event()
    dut1_started_early = threading.Event()

    def watch_dut1():
        while not dut1_turn.is_set():
            if dut.app_running():
                dut1_started_early.set()
            time.sleep(0.5)

    def log_turns(text):
        log(text)
        if "STATUS 0x30" in text and text.startswith("Updating"):
            dut1_turn.set()

    watcher = threading.Thread(target=watch_dut1)
    reset_soon(dut)
    time.sleep(1.0)  # past the reset: the watcher would see the old application before it
    watcher.start()
    try:
        outcomes = update_nodes(
            bus,
            [Node(IDS2, big_reboot, version2), Node(IDS1, firmware.blinky_fast, 0x0101)],
            wait_timeout=2 * DUT2_CYCLE_S,
            log=log_turns,
        )
    finally:
        dut1_turn.set()
        watcher.join()
    assert outcomes == [Outcome.UPDATED, Outcome.UPDATED]
    assert not dut1_started_early.is_set()
    _assert_dut1(dut, firmware.blinky_fast, 0x0101)
    _assert_dut2(bus, version2)


def test_cli_update_nodes(dut, firmware, blinky_reboot, adapter_port, tmp_path):
    # The CLI opens the adapter itself: only open it here for finding out the second DUT's version.
    card = UsbLinCard(adapter_port, name="adapter")
    try:
        status = _dut2_status(CardCommanderBus(card), 2 * DUT2_CYCLE_S)
    finally:
        card.close_channel()
        card.wipe()
        card.close()
    if status is None:
        pytest.skip("no second RESPONDER DUT (STATUS 0x20) on the bus")
    version2 = _next_version(status.firmware_version)
    dut.install(firmware.blinky_slow, 0x0100)
    app1 = tmp_path / "app1.bin"
    app1.write_bytes(firmware.blinky_fast)
    app2 = tmp_path / "app2.bin"
    app2.write_bytes(blinky_reboot)
    result = run_tool(
        adapter_port,
        "update-nodes",
        "--node",
        f"0x30,0x31,0x32,{app1},2.1",
        "--node",
        f"0x20,0x21,0x22,{app2},{version2 >> 8}.{version2 & 0xFF}",
        "--wait",
        str(2 * DUT2_CYCLE_S),
        reset_dut=dut,
    )
    assert result.returncode == 0
    assert result.stdout.count("Result: updated") == 2
    _assert_dut1(dut, firmware.blinky_fast, 0x0201)
