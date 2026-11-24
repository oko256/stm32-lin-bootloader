"""Access to the device under test over SWD, with STM32_Programmer_CLI (STM32CubeProgrammer)."""

import re
import subprocess
import tempfile
from pathlib import Path

_ANSI = re.compile(r"\x1b\[[0-9;]*m")


class SwdError(Exception):
    pass


class Swd:
    def __init__(self, cli: str):
        self.cli = cli

    def run(self, *args: str, mode: str = "HOTPLUG") -> str:
        """Runs the programmer with the given commands; returns its output (without colors)."""
        cmd = [self.cli, "-c", "port=SWD", f"mode={mode}", *args]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        output = _ANSI.sub("", result.stdout + result.stderr)
        if result.returncode != 0 or "Error:" in output:
            raise SwdError(f"{' '.join(cmd)} failed:\n{output}")
        return output

    def reset(self) -> None:
        """Resets the device (hardware reset) and lets it run."""
        self.run("-hardRst")

    def flash(self, path: Path, erase_all: bool = False) -> None:
        """Programs an ELF (or hex) file, optionally erasing the whole flash first, and resets."""
        args = ["-e", "all"] if erase_all else []
        self.run(*args, "-d", str(path), "-v", "-rst", mode="UR")

    # Flash operations are done with the core held in reset (the running firmware may use the flash
    # interface too); the device starts running from reset afterwards.

    def write(self, data: bytes, addr: int) -> None:
        """Programs raw data at addr (the touched pages are erased first)."""
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(data)
            f.flush()
            self.run("-w", f.name, f"0x{addr:08X}", "-v", mode="UR")

    def erase_pages(self, first: int, last: int) -> None:
        self.run("-e", f"[{first} {last}]", mode="UR")

    def read(self, addr: int, size: int) -> bytes:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "read.bin"
            self.run("-u", f"0x{addr:08X}", str(size), str(path))
            return path.read_bytes()

    def read32(self, *addrs: int) -> list[int]:
        """Reads one 32-bit word at each address (in one programmer run)."""
        args = []
        for addr in addrs:
            args += ["-r32", f"0x{addr:08X}", "4"]
        output = self.run(*args)
        values = []
        for addr in addrs:
            m = re.search(rf"^0x{addr:08X}\s*:\s*([0-9A-Fa-f]{{8}})", output, re.M)
            if m is None:
                raise SwdError(f"no value for 0x{addr:08X} in:\n{output}")
            values.append(int(m.group(1), 16))
        return values

    def option_bytes(self) -> dict[str, int]:
        output = self.run("-ob", "displ", mode="UR")
        return {m.group(1): int(m.group(2), 16) for m in re.finditer(r"^\s*(\w+)\s*:\s*0x([0-9A-Fa-f]+)", output, re.M)}

    def set_option_bytes(self, **values: int) -> None:
        self.run("-ob", *(f"{k}={v}" for k, v in values.items()), mode="UR")
