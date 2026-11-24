"""System tests: a USB LIN Card with the bootloader as the device under test (DUT), updated over
LIN through another USB LIN Card (the adapter, stock firmware). The DUT is flashed, reset and
inspected over SWD with STM32_Programmer_CLI. See README.md (System tests)."""

import os
import random
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools" / "linboot"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from dut import APP_MAX_SIZE, Dut  # noqa: E402
from swd import Swd  # noqa: E402
from usblincard import UsbLinCard  # noqa: E402

DEFAULT_ADAPTER = "/dev/serial/by-id/usb-oko256_USB_LIN_Card_204E357E4830-if00"
DEFAULT_ADAPTER2 = "/dev/serial/by-id/usb-oko256_USB_LIN_Card_203DB55B4830-if00"
DEFAULT_CLI = "~/STM32CubeProg/bin/STM32_Programmer_CLI"


def pytest_addoption(parser):
    parser.addoption(
        "--adapter",
        default=os.environ.get("LINBOOT_ADAPTER", DEFAULT_ADAPTER),
        help="serial port of the adapter USB LIN Card (env LINBOOT_ADAPTER)",
    )
    parser.addoption(
        "--adapter2",
        default=os.environ.get("LINBOOT_ADAPTER2", DEFAULT_ADAPTER2),
        help="serial port of a second adapter, for the tests with two providers (env LINBOOT_ADAPTER2)",
    )
    parser.addoption(
        "--programmer",
        default=os.environ.get("STM32_PROGRAMMER_CLI", DEFAULT_CLI),
        help="path of STM32_Programmer_CLI (env STM32_PROGRAMMER_CLI)",
    )
    parser.addoption("--no-build", action="store_true", help="don't (re)build the firmware, use what's in build/")


@dataclass
class Firmware:
    """Build products of one role (usb-lin-card-<role>-release preset)."""

    bootloader: Path
    blinky_slow: bytes
    blinky_fast: bytes


@pytest.fixture(scope="session")
def build(request):
    """Returns a function that builds (once) and returns the firmware of a role."""
    cache: dict[str, Firmware] = {}

    def get(role: str) -> Firmware:
        if role not in cache:
            preset = f"usb-lin-card-{role}-release"
            if not request.config.getoption("--no-build"):
                for cmd in (["cmake", "--preset", preset], ["cmake", "--build", "--preset", preset]):
                    subprocess.run(cmd, cwd=REPO, check=True, capture_output=True)
            out = REPO / "build" / preset
            cache[role] = Firmware(
                bootloader=out / "linboot.elf",
                blinky_slow=(out / "examples" / "blinky" / "blinky-slow.bin").read_bytes(),
                blinky_fast=(out / "examples" / "blinky" / "blinky-fast.bin").read_bytes(),
            )
        return cache[role]

    return get


@pytest.fixture(scope="session")
def swd(request) -> Swd:
    swd = Swd(os.path.expanduser(request.config.getoption("--programmer")))
    # The DUT must boot from the flash; the bootloader could fix it itself, but only if it runs.
    ob = swd.option_bytes()
    if ob.get("nBOOT0") != 1 or ob.get("BOOT_SEL") != 0:
        swd.set_option_bytes(nBOOT0=1, BOOT_SEL=0)
    return swd


@pytest.fixture(scope="session")
def dut(swd) -> Dut:
    return Dut(swd)


@pytest.fixture
def adapter_port(request) -> str:
    return request.config.getoption("--adapter")


def _open_card(port: str, name: str):
    """Opens an adapter for one test. It starts and ends closed with empty tables, so that it doesn't
    answer anything by itself."""
    card = UsbLinCard(port, name)

    def clean():
        card.cmd("C")
        card.cmd_ok("w")
        card.clear_frames()
        card.clear_replies()

    clean()
    yield card
    clean()
    card.close()


@pytest.fixture
def card(adapter_port):
    """The adapter (the CLI tests open the port themselves instead)."""
    yield from _open_card(adapter_port, "adapter")


@pytest.fixture
def card2(request):
    """A second adapter; the test is skipped if there's none."""
    port = request.config.getoption("--adapter2")
    if not Path(port).exists():
        pytest.skip(f"no second adapter at {port}")
    yield from _open_card(port, "adapter2")


def reset_soon(dut: Dut, delay: float = 0.3) -> threading.Timer:
    """Resets the DUT after delay, while the caller starts waiting for the bootloader."""
    timer = threading.Timer(delay, dut.reset)
    timer.start()
    return timer


def wait_until(condition, timeout: float, interval: float = 0.1) -> bool:
    end = time.monotonic() + timeout
    while True:
        if condition():
            return True
        if time.monotonic() >= end:
            return False
        time.sleep(interval)


def run_tool(adapter: str, *args: str, reset_dut: Dut | None = None, timeout: float = 300) -> subprocess.CompletedProcess:
    """Runs tools/linboot/linboot.py; resets the DUT once the tool has started (if given)."""
    cmd = [sys.executable, str(REPO / "tools" / "linboot" / "linboot.py"), "-p", adapter, *args]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if reset_dut is not None:
        time.sleep(1.5)
        reset_dut.reset()
    output, _ = proc.communicate(timeout=timeout)
    print(output)
    return subprocess.CompletedProcess(cmd, proc.returncode, output)


@pytest.fixture(scope="session")
def big_image(build) -> bytes:
    """A 20 KB application: blinky-slow padded with random data (it still runs normally)."""
    blinky = build("responder").blinky_slow
    image = blinky + random.Random(1).randbytes(20 * 1024 - len(blinky))
    assert len(image) <= APP_MAX_SIZE
    return image


class Log:
    """Collects the log lines of the host tool (and prints them, shown on failures)."""

    def __init__(self):
        self.lines: list[str] = []

    def __call__(self, text: str):
        print(text)
        self.lines.append(text)

    def has(self, text: str) -> bool:
        return any(text in line for line in self.lines)


@pytest.fixture
def log() -> Log:
    return Log()
