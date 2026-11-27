#!/usr/bin/env python3
"""Host tool for the STM32 LIN bootloader, using a USB LIN Card.

  update   The host is the bus commander and updates a RESPONDER role bootloader.
  serve    The host is a provider (responder) for a COMMANDER role bootloader.
  update-nodes  Like update, for several RESPONDER role bootloaders on the bus at once.
  status   Reads the STATUS of a RESPONDER role bootloader once.

The frame IDs default to the defaults of the bootloader's CMake options.
"""

import argparse
import sys
from pathlib import Path

import protocol as p
from bus import CardCommanderBus, CardResponderBus
from provider import CommanderIds, Provider
from updater import Node, ResponderIds, UpdateError, Updater, update_nodes
from usblincard import UsbLinCard


def _int(text: str) -> int:
    return int(text, 0)


def _id_list(text: str) -> list[int]:
    return [int(part, 0) for part in text.replace(";", ",").split(",") if part.strip()]


class _Console:
    """Prints the transfer progress on one line, and log messages on their own lines."""

    def __init__(self):
        self.last = -1
        self.mid_line = False

    def progress(self, done: int, total: int):
        percent = done * 100 // total
        if percent != self.last:
            self.last = percent
            print(f"\r{done}/{total} bytes ({percent} %)", end="", flush=True)
            self.mid_line = True

    def log(self, text: str):
        if self.mid_line:
            print()
            self.mid_line = False
        print(text, flush=True)


def _read_image(path: Path) -> bytes:
    image = path.read_bytes()
    if not image:
        raise SystemExit(f"{path} is empty")
    return image


def cmd_update(args, card: UsbLinCard) -> int:
    image = _read_image(args.image)
    bus = CardCommanderBus(card, args.baud, args.classic_checksum)
    ids = ResponderIds(args.status_id, args.cmd_id, args.data_id)
    console = _Console()
    updater = Updater(bus, ids, attempts=args.attempts, log=console.log, progress=console.progress)
    print(f"Waiting for the bootloader (STATUS 0x{ids.status:02X}) for {args.wait} s: reset the device now")
    try:
        outcome = updater.update(
            image,
            args.version,
            force=args.force,
            wait_timeout=args.wait,
            device_type=args.device_type,
            hardware_version=args.hw_version,
            boot=not args.no_boot,
        )
    except UpdateError as e:
        print(f"Update failed: {e}", file=sys.stderr)
        return 1
    console.log(f"Result: {outcome.value}")
    return 0


def _node(text: str) -> Node:
    """Parses "STATUS_ID,CMD_ID,DATA_ID,IMAGE,VERSION[,force]"."""
    parts = text.split(",")
    if len(parts) not in (5, 6) or (len(parts) == 6 and parts[5] != "force"):
        raise argparse.ArgumentTypeError(f"expected STATUS_ID,CMD_ID,DATA_ID,IMAGE,VERSION[,force], got {text!r}")
    ids = ResponderIds(int(parts[0], 0), int(parts[1], 0), int(parts[2], 0))
    return Node(ids, _read_image(Path(parts[3])), p.parse_version(parts[4]), force=len(parts) == 6)


def cmd_update_nodes(args, card: UsbLinCard) -> int:
    bus = CardCommanderBus(card, args.baud, args.classic_checksum)
    console = _Console()
    ids = ", ".join(f"0x{n.ids.status:02X}" for n in args.node)
    print(f"Waiting for the bootloaders (STATUS {ids}) for {args.wait} s: reset the devices now")
    try:
        outcomes = update_nodes(
            bus, args.node, wait_timeout=args.wait, attempts=args.attempts, log=console.log, progress=console.progress
        )
    except UpdateError as e:
        print(f"Update failed: {e}", file=sys.stderr)
        return 1
    for node, outcome in zip(args.node, outcomes, strict=True):
        console.log(f"STATUS 0x{node.ids.status:02X} Result: {outcome.value}")
    return 0


def cmd_serve(args, card: UsbLinCard) -> int:
    image = _read_image(args.image)
    bus = CardResponderBus(card, args.baud, args.classic_checksum)
    ids = CommanderIds(args.query_id, args.data_id, args.offer_ids)
    console = _Console()
    provider = Provider(
        bus,
        image,
        args.version,
        force=args.force,
        index=args.index,
        ids=ids,
        device_type=args.device_type,
        hardware_version=args.hw_version,
        log=console.log,
        progress=console.progress,
    )
    print(
        f"Offering {args.image} ({len(image)} bytes) as version {p.format_version(args.version)} "
        f"in OFFER 0x{ids.offers[args.index]:02X}; reset the device to start"
    )
    result = provider.run(timeout=args.timeout, until_done=not args.forever)
    return 0 if result == p.Error.NONE else 1


def cmd_status(args, card: UsbLinCard) -> int:
    bus = CardCommanderBus(card, args.baud, args.classic_checksum)
    updater = Updater(bus, ResponderIds(status=args.status_id))
    status = updater.read_status(args.wait)
    if status is None:
        print("No response")
        return 1
    print(status.describe())
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", "-p", required=True, help="serial port of the USB LIN Card, e.g. /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=19200, help="LIN baud rate (default: %(default)s)")
    parser.add_argument("--classic-checksum", action="store_true", help="use the classic checksum")
    sub = parser.add_subparsers(dest="command", required=True)

    def add_image_args(sp):
        sp.add_argument("image", type=Path, help="application image (.bin)")
        sp.add_argument("--version", "-v", type=p.parse_version, required=True, help="firmware version, e.g. 1.2")
        sp.add_argument("--force", "-f", action="store_true", help="install even if the version is already installed")
        sp.add_argument("--device-type", type=_int, help="only update this device type")
        sp.add_argument("--hw-version", type=_int, help="only update this hardware version")

    sp = sub.add_parser("update", help="update a RESPONDER role bootloader (the host is the commander)")
    add_image_args(sp)
    sp.add_argument("--status-id", type=_int, default=0x30, help="STATUS frame ID (default: 0x30)")
    sp.add_argument("--cmd-id", type=_int, default=0x31, help="CMD frame ID (default: 0x31)")
    sp.add_argument("--data-id", type=_int, default=0x32, help="DATA frame ID (default: 0x32)")
    sp.add_argument("--wait", type=float, default=60.0, help="how long to wait for the bootloader (s)")
    sp.add_argument("--attempts", type=int, default=3, help="update attempts (default: %(default)s)")
    sp.add_argument("--no-boot", action="store_true", help="don't send BOOT at the end")
    sp.set_defaults(func=cmd_update)

    sp = sub.add_parser("update-nodes", help="update several RESPONDER role bootloaders (the host is the commander)")
    sp.add_argument(
        "--node",
        type=_node,
        action="append",
        required=True,
        help="STATUS_ID,CMD_ID,DATA_ID,IMAGE,VERSION[,force] of one node (repeat for each, updated in this order)",
    )
    sp.add_argument("--wait", type=float, default=60.0, help="how long to wait for all the bootloaders (s)")
    sp.add_argument("--attempts", type=int, default=3, help="update attempts per node (default: %(default)s)")
    sp.set_defaults(func=cmd_update_nodes)

    sp = sub.add_parser("serve", help="provide an update for a COMMANDER role bootloader")
    add_image_args(sp)
    sp.add_argument("--index", type=int, default=0, help="provider index (position in the OFFER ID list)")
    sp.add_argument("--query-id", type=_int, default=0x31, help="QUERY frame ID (default: 0x31)")
    sp.add_argument("--data-id", type=_int, default=0x32, help="DATA frame ID (default: 0x32)")
    sp.add_argument(
        "--offer-ids",
        type=_id_list,
        default=[0x33, 0x34, 0x35, 0x36],
        help="OFFER frame IDs (default: 0x33,0x34,0x35,0x36)",
    )
    sp.add_argument("--timeout", type=float, help="give up after this many seconds")
    sp.add_argument("--forever", action="store_true", help="keep serving after a successful update")
    sp.set_defaults(func=cmd_serve)

    sp = sub.add_parser("status", help="read the STATUS of a RESPONDER role bootloader")
    sp.add_argument("--status-id", type=_int, default=0x30, help="STATUS frame ID (default: 0x30)")
    sp.add_argument("--wait", type=float, default=1.0, help="how long to poll (s)")
    sp.set_defaults(func=cmd_status)

    args = parser.parse_args(argv)
    card = UsbLinCard(args.port, "card")
    try:
        return args.func(args, card)
    except KeyboardInterrupt:
        return 130
    finally:
        # Leave the card quiet: otherwise it keeps publishing our responses (e.g. the OFFER).
        try:
            card.cmd("C", timeout=0.5)
            card.cmd("w", timeout=0.5)
        except TimeoutError:
            pass
        card.close()


if __name__ == "__main__":
    sys.exit(main())
