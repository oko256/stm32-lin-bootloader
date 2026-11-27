"""The device under test: a USB LIN Card (STM32F042G6U6) with the bootloader, seen over SWD.

The constants follow the usb-lin-card presets (default layout and IDs).
"""

import time
import zlib
from dataclasses import dataclass

from swd import Swd

FLASH_BASE = 0x08000000
PAGE_SIZE = 1024
APP_ADDR = 0x08001000
META_ADDR = 0x08007C00
APP_MAX_SIZE = META_ADDR - APP_ADDR
APP_FIRST_PAGE = (APP_ADDR - FLASH_BASE) // PAGE_SIZE
META_PAGE = (META_ADDR - FLASH_BASE) // PAGE_SIZE

DEVICE_TYPE = 0x0001
HW_VERSION = 0x0003

SYSCFG_CFGR1 = 0x40010000
SCB_ICSR = 0xE000ED04
GPIOA_ODR = 0x48000014
LED_BIT = 1 << 7  # PA7

META_MAGIC = 0x314D424C
META_SIZE = 24


def meta_record(version: int, image: bytes) -> bytes:
    """The metadata record the bootloader writes after a successful update (src/core/meta.h)."""
    body = (
        META_MAGIC.to_bytes(4, "little")
        + version.to_bytes(2, "little")
        + b"\xff\xff"
        + len(image).to_bytes(4, "little")
        + zlib.crc32(image).to_bytes(4, "little")
        + b"\xff\xff\xff\xff"
    )
    return body + zlib.crc32(body).to_bytes(4, "little")


@dataclass
class Installed:
    version: int
    size: int
    crc: int


class Dut:
    def __init__(self, swd: Swd):
        self.swd = swd

    def reset(self) -> None:
        self.swd.reset()

    def install(self, image: bytes, version: int) -> None:
        """Puts an application in place directly over SWD, as if it had been updated, and waits
        until the bootloader has started it (programming resets the device, and the tests must not
        catch the bootloader in that boot window by accident)."""
        self.swd.erase_pages(APP_FIRST_PAGE, META_PAGE)
        self.swd.write(image, APP_ADDR)
        self.swd.write(meta_record(version, image), META_ADDR)
        end = time.monotonic() + 5.0
        while not self.app_running():
            if time.monotonic() > end:
                raise AssertionError("the installed application didn't start")
            time.sleep(0.1)

    def erase_app(self) -> None:
        """Erases the application and its metadata: the bootloader has nothing to boot."""
        self.swd.erase_pages(APP_FIRST_PAGE, META_PAGE)

    def installed(self) -> Installed | None:
        """Returns the installed application according to the metadata, if the record is valid."""
        record = self.swd.read(META_ADDR, META_SIZE)
        if int.from_bytes(record[0:4], "little") != META_MAGIC:
            return None
        if int.from_bytes(record[20:24], "little") != zlib.crc32(record[:20]):
            return None
        return Installed(
            version=int.from_bytes(record[4:6], "little"),
            size=int.from_bytes(record[8:12], "little"),
            crc=int.from_bytes(record[12:16], "little"),
        )

    def app_image(self, size: int) -> bytes:
        return self.swd.read(APP_ADDR, size)

    def app_running(self) -> bool:
        """The bootloader remaps the RAM to address 0 (MEM_MODE = 3) right before starting the
        application; the bootloader itself runs with the flash at 0."""
        (cfgr1,) = self.swd.read32(SYSCFG_CFGR1)
        return (cfgr1 & 3) == 3

    def led_toggles(self, duration: float = 1.5) -> bool:
        """Samples the LED pin: both blinky variants toggle it at least once per second."""
        samples = []
        end = time.monotonic() + duration
        while time.monotonic() < end and len(set(samples)) < 2:
            (odr,) = self.swd.read32(GPIOA_ODR)
            samples.append(int(bool(odr & LED_BIT)))
            time.sleep(0.05)
        if len(set(samples)) < 2:
            icsr, cfgr1 = self.swd.read32(SCB_ICSR, SYSCFG_CFGR1)
            print(
                f"LED not toggling: samples {samples}, ICSR 0x{icsr:08X} (active exception {icsr & 0x3F}), "
                f"SYSCFG_CFGR1 0x{cfgr1:08X}"
            )
            return False
        return True
