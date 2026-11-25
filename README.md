# STM32 LIN bootloader

A small bootloader for STM32 microcontrollers that updates the application in the flash over a LIN
bus. It is written in plain C on top of the CMSIS headers (no HAL), and everything about it is
configured with CMake options.

The bootloader can be built for one of two LIN roles:

- **RESPONDER:** the device is a LIN responder (slave), and the bus **commander updates it**.
- **COMMANDER:** the device is the LIN commander (master), and it **pulls its update from one of
  the responders** on the bus (a "provider").

LIN protocol handling is done by the [minlin](https://github.com/oko256/minlin) library.

The first port is the STM32F0 (Cortex-M0), tested in builds on the STM32F042G6U6 of the
[USB LIN Card](https://github.com/oko256/usb-lin-card-hw). The core is platform independent, so
other STM32 families need only a new port (see [Porting](#porting)).

Some of the files in this repository (`Drivers/CMSIS`) are under ARM's and ST's licenses, and those
files keep their respective licenses regardless of the root LICENSE file.

**Status:** both roles work on hardware, tested with automated system tests (a USB LIN Card as
the device, updated through another USB LIN Card; see [System tests](#system-tests)). The
bootloader, the protocol and the host tool are also covered by host tests, including end-to-end
tests of the host tool against the real state machines.

# User guide

## How it works

After a reset, the bootloader always runs first. It reads the metadata of the installed application
(version, size, CRC32) and checks the CRC of the application in the flash. Then it looks for an
update for `LINBOOT_BOOT_TIMEOUT_MS` (2 s by default):

- **RESPONDER:** it waits for the commander to poll its STATUS frame (the "heartbeat"). If nothing
  comes, it starts the application. If the commander does poll it, the bootloader stays until the
  commander tells it to boot, or until the bus has been quiet for `LINBOOT_SESSION_TIMEOUT_MS`.
- **COMMANDER:** it broadcasts its identity (device type, hardware version and firmware version)
  and polls the OFFER frame of each possible provider. If a provider has a suitable firmware, it is
  downloaded. If providers answer but have nothing new, the application is started right away; if
  nobody answers, it is started after the timeout.

Without a valid application, the bootloader never times out: it keeps waiting (or looking) for an
update.

**When is an offered firmware installed?** When its version differs from the installed one, or
when there is no valid application. The updater can also set a **FORCE** flag to reinstall the same
version (e.g. during development). In the COMMANDER role, the highest offered version is chosen
(the first provider of equal ones), so providers with different versions can't make the device
flip-flop between them. An offer with FORCE wins over all others. A provider whose transfer
failed (e.g. its image doesn't match its CRC) is skipped in the next rounds while others offer
something, so a broken provider can't keep the device from getting a working image; once all
the offering providers have failed, they all get tried again. The choice is made from two discovery
rounds: a provider may start offering only when it sees the first IDENTIFY, too late for its OFFER
slot of that round.

**Safety:** the metadata page is erased right before the first block of the new image is written,
and written again only after the CRC of the whole image in the flash has been verified. A
power-loss or a failed transfer at any point leaves the device in the bootloader, waiting for a new
update. The bootloader refuses to write to its own flash area.

The identifying details of the device:

- **device_type** (u16): what kind of device this is; CMake option `LINBOOT_DEVICE_TYPE`.
- **hardware_version** (u16): returned by the hook `linboot_hook_hardware_version()`, which the
  integrator implements for the board (version resistors, ADC, EEPROM, ...).
- **firmware_version** (u16): version of the installed application, major in the high byte and
  minor in the low byte. Comes with the update and is stored in the metadata page.

## Building

Requirements: CMake 3.28+, Ninja and the GNU Arm Embedded toolchain (`arm-none-eabi-gcc`).

```bash
git clone --recurse-submodules <repo>
cmake --preset usb-lin-card-responder-release
cmake --build --preset usb-lin-card-responder-release
```

This builds `build/<preset>/linboot.elf` (and `.bin`, `.hex`) and the blinky example
applications in `build/<preset>/examples/blinky/`. The presets are for the USB LIN Card board:
`usb-lin-card-{responder,commander}-{debug,release}`. For other targets, give the options on the
command line or make a preset that inherits `arm`, e.g.:

```bash
cmake -B build/my-device -G Ninja --toolchain cmake/arm-none-eabi.cmake \
    -DLINBOOT_ROLE=RESPONDER -DLINBOOT_DEVICE_TYPE=0x0042 \
    -DLINBOOT_ID_STATUS=0x20 -DLINBOOT_ID_CMD=0x21 -DLINBOOT_ID_DATA=0x22 \
    -DLINBOOT_EXTRA_SOURCES=$PWD/../my-board/linboot_board.c
```

The configuration summary (role, layout) is printed when configuring. The bootloader is about
3.4 KB in the RESPONDER role and 3.9 KB in the COMMANDER role (with the USB LIN Card options), so it
fits in the default 4 KB. If your hooks need more, increase `LINBOOT_BOOTLOADER_SIZE`; the linker
reports an overflow.

Flash the bootloader with any SWD programmer, e.g.
`STM32_Programmer_CLI -c port=SWD -w build/<preset>/linboot.elf -rst`.

## Host tool

`tools/linboot/linboot.py` updates devices through a [USB LIN Card](https://github.com/oko256/usb-lin-card-fw).
It needs Python 3.10+ and pyserial. The frame IDs default to the defaults of the CMake options.

```bash
# RESPONDER role: the PC is the commander. Start this, then reset the device.
tools/linboot/linboot.py -p /dev/ttyACM0 update build/<preset>/examples/blinky/blinky-fast.bin --version 1.1
tools/linboot/linboot.py -p /dev/ttyACM0 update app.bin --version 1.1 --force   # reinstall the same version
tools/linboot/linboot.py -p /dev/ttyACM0 status                                 # read the STATUS once

# Several RESPONDER nodes: all are caught in their bootloaders first, then updated in this order.
tools/linboot/linboot.py -p /dev/ttyACM0 update-nodes \
    --node 0x30,0x31,0x32,app1.bin,1.1 --node 0x20,0x21,0x22,app2.bin,2.0,force

# COMMANDER role: the PC is provider 0. Start this, then reset the device.
tools/linboot/linboot.py -p /dev/ttyACM0 serve app.bin --version 1.1 --index 0
```

With several nodes, the nodes waiting for their turn have their heartbeat (STATUS) polled every
second, so that they stay in their bootloaders instead of starting their applications after the
session timeout. A real commander updating several nodes must do the same.

The card needs the LIN bus supply (V+), and the commander side (the card or the device) needs its
MASTER pull-up jumper fitted.

Serving a COMMANDER device is about three times faster with card firmware 6 or later, which
`linboot.py` uses automatically (see [performance](#performance)).

## Protocol

All bootloader frames carry **8 bytes** and use the enhanced checksum (`LINBOOT_CLASSIC_CHECKSUM`
switches to the classic one). Multi-byte fields are little-endian. The frame IDs are fixed at
compile time (CMake options); IDs 0x3C..0x3F are not allowed.

- **CRC:** CRC-32/ISO-HDLC, the same as `zlib.crc32()`, over the image bytes.
- **Image:** the raw binary of the application (the `.bin`), starting at the application address,
  at most 2^24 - 1 bytes.
- **Data chunks:** a DATA frame is `[seq][7 image bytes]`. Chunk *k* covers the image offset 7*k*,
  and *seq* = *k* & 0xFF. The sequence number lets the receiver detect lost frames, and the
  COMMANDER role bootloader detect stale data from a slow provider. Bytes past the end are padding.

### RESPONDER role

Each node has its own three IDs: `LINBOOT_ID_STATUS` (default 0x30), `LINBOOT_ID_CMD` (0x31) and
`LINBOOT_ID_DATA` (0x32).

| Frame | Published by | Payload |
|---|---|---|
| STATUS | bootloader | Depends on the state, see below. Its header is the heartbeat. |
| CMD | commander | `[cmd][arguments]`, see below. |
| DATA | commander | `[seq][7 image bytes]` |

**STATUS** (byte 0 is the state):

| State | Code | Layout |
|---|---|---|
| IDLE | 0x01 | `[state][device_type:2][hw_version:2][fw_version:2][flags]`; flags: bit 0 = valid application, bits 7..4 = protocol version (1) |
| REJECTED | 0x02 | Like IDLE. The offer was not taken: that version is already installed. |
| RECEIVING | 0x10 | `[state][error][next_offset:3][window_end:3]` |
| VERIFYING | 0x11 | Like RECEIVING (window_end 0). |
| DONE | 0x20 | `[state][error][fw_version:2][0 0 0 0]`: installed, boots after `LINBOOT_BOOT_DELAY_MS` (100 ms) or on BOOT. |
| FAILED | 0x21 | Like DONE (fw_version is the installed one, 0 if none). |

**CMD:**

| Command | Code | Layout |
|---|---|---|
| OFFER | 0x01 | `[0x01][fw_version:2][image_size:3][flags][0]`; flags: bit 0 = FORCE |
| FINISH | 0x02 | `[0x02][crc32:4][0 0 0]`: all data sent, verify |
| ABORT | 0x03 | `[0x03][0...]`: back to IDLE |
| BOOT | 0x04 | `[0x04][0...]`: start the application (if valid, and no transfer is going on) |

**Error codes:** 0 none, 1 bad size (empty or too large), 2 flash failure, 3 CRC mismatch, 4 bad
state (e.g. FINISH before all data), 5 timeout (COMMANDER role).

**Update sequence** (the commander side, as done by `tools/linboot/updater.py`):

1. Poll STATUS until the bootloader responds (it does so only for `LINBOOT_BOOT_TIMEOUT_MS` after
   a reset, unless there is no valid application). Check the device type and hardware version.
2. Send OFFER. Read STATUS: REJECTED means already up to date (send BOOT), RECEIVING means go.
3. Read STATUS, send the DATA chunks from `next_offset` up to (not including) `window_end`, repeat.
   When `window_end` equals `next_offset`, the bootloader is busy writing a block to the flash, and
   it doesn't respond to STATUS while the flash is busy (tens of milliseconds): just poll again.
   Lost chunks are no problem: the bootloader ignores chunks out of sequence, and `next_offset`
   tells where to continue.
4. When `next_offset` reaches the image size, send FINISH with the CRC, and poll STATUS until DONE
   or FAILED. After FAILED, start again from OFFER (a few attempts).
5. Send BOOT (or let the boot delay expire).

### COMMANDER role

IDs: `LINBOOT_ID_QUERY` (default 0x31; published by the bootloader, subscribed by all providers),
`LINBOOT_ID_DATA` (0x32; published by the selected provider), and `LINBOOT_PROVIDER_OFFER_IDS`
(0x33;0x34;0x35;0x36), one OFFER ID per possible provider; the provider index is its position in
the list. Providers are typically the applications of other nodes on the bus.

| Frame | Published by | Payload |
|---|---|---|
| QUERY IDENTIFY | bootloader | `[0x01][device_type:2][hw_version:2][fw_version:2][flags]` (flags like in STATUS) |
| QUERY READ | bootloader | `[0x02][provider_index][offset:3][0 0 0]` |
| QUERY RESULT | bootloader | `[0x03][provider_index][error][0...]` |
| OFFER_k | provider k | `[fw_version:2][image_size:3][flags][0 0]`; flags: bit 0 = FORCE. Only if it has an image for the identity in the last IDENTIFY. |
| DATA | selected provider | `[seq][7 bytes]` of the data stream: the image followed by its CRC32 (4 bytes) |

**What a provider does:**

- On IDENTIFY: publish OFFER_k if it has a firmware for that device type and hardware version.
- On READ for its index: serve the stream chunk at `offset` in DATA. After each published DATA
  response, advance to the next chunk. If the provider can't tell when its response was sent
  (like the USB LIN Card), it just keeps serving the chunk of the last READ: the bootloader notices
  the stale sequence number and sends a READ for the next chunk.
- On a READ for another provider, or a new IDENTIFY: stop responding to DATA (another provider
  may be selected now, and two responses would collide).
- On RESULT: the update is done (error 0) or failed; nothing needs to be done.

**Bootloader behaviour:**

1. Send IDENTIFY, then the header of each OFFER_k. Repeat until the boot timeout (forever if there
   is no valid application). After the first round that got an offer, do one more round; the
   offers of both rounds count.
2. Choose an offer (see [How it works](#how-it-works)), or boot.
3. Send READ(k, 0) and poll DATA. Accept a chunk with the expected sequence number. Otherwise, poll
   again up to `LINBOOT_POLL_RETRIES` times, or right away send READ for the expected chunk if the
   provider serves some other chunk. After `LINBOOT_READ_RETRIES` READs without progress, the
   attempt fails.
4. After the stream, verify the flash CRC against the trailer, write the metadata, send RESULT and
   boot.
5. A failed attempt sends RESULT with the error and starts over from IDENTIFY (skipping that
   provider while others offer, see above). After
   `LINBOOT_MAX_ATTEMPTS` failed attempts, the application is booted if it's still intact
   (nothing was written yet); otherwise the bootloader keeps trying.

### Performance

At 19200 baud, an 8-byte frame takes about 7 ms (the bootloader's slot ends when the response is
complete; without a response, after 11 ms). So when the DATA frames are polled back to back, the
bus carries about 1000 image bytes per second: a full 27 KB application takes about 30 s.

Measured with a USB LIN Card as the updater, 19200 baud, 20 KB image:

- **RESPONDER role** (`linboot.py update`): 41 s, about 500 B/s. The card sends each DATA frame as
  a single frame (`t0`), whose slot is the card's worst-case frame time (13 ms for 8 bytes).
- **COMMANDER role** (`linboot.py serve`), card firmware 6 or later: 21 s, about 980 B/s.
  - On a READ, the tool replaces the DATA response (`t2`) and queues the next 16 chunks (`t3`).
  - The card reports each published chunk, and the tool keeps the queue full.
  - So the bootloader polls DATA back to back, and sends a READ only to resynchronize.
- **COMMANDER role, older card firmware:** 59 s, about 340 B/s.
  - The card can't advance its DATA response by itself, so each chunk takes three frames: a READ,
    the DATA frame, and a stale DATA frame that triggers the next READ.
  - The tool rewrites the card's responder table for each READ.

## CMake options

Numbers can be decimal or 0x-prefixed hex.

| Option | Default | Description |
|---|---|---|
| `LINBOOT_ROLE` | RESPONDER | RESPONDER or COMMANDER |
| `LINBOOT_DEVICE_TYPE` | (required) | Device type (u16) |
| `LINBOOT_HW_VERSION_DEFAULT` | 0 | Returned by the default hardware version hook |
| `LINBOOT_EXTRA_SOURCES` | | Extra sources, e.g. your hook implementations |
| `LINBOOT_EXTRA_INCLUDE_DIRS` | | Extra include directories |
| `LINBOOT_BAUD` | 19200 | LIN baud rate |
| `LINBOOT_CLASSIC_CHECKSUM` | OFF | Classic (LIN 1.x) checksum |
| `LINBOOT_ID_STATUS`, `_CMD`, `_DATA` | 0x30, 0x31, 0x32 | RESPONDER role frame IDs (DATA also for COMMANDER) |
| `LINBOOT_ID_QUERY` | 0x31 | COMMANDER role QUERY frame ID |
| `LINBOOT_PROVIDER_OFFER_IDS` | 0x33;0x34;0x35;0x36 | COMMANDER role OFFER frame IDs, 1 to 16 |
| `LINBOOT_BOOT_TIMEOUT_MS` | 2000 | How long to look for an update after reset |
| `LINBOOT_SESSION_TIMEOUT_MS` | 5000 | RESPONDER: give up a session after this long without traffic |
| `LINBOOT_BOOT_DELAY_MS` | 100 | RESPONDER: delay from DONE to booting |
| `LINBOOT_MAX_ATTEMPTS` | 3 | COMMANDER: update attempts |
| `LINBOOT_POLL_RETRIES` | 3 | COMMANDER: DATA polls before repeating READ |
| `LINBOOT_READ_RETRIES` | 20 | COMMANDER: READs without progress before failing |
| `LINBOOT_PORT` | stm32f0 | Platform port |
| `LINBOOT_MCU` | STM32F042x6 | CMSIS device define; sets the memory size defaults |
| `LINBOOT_FLASH_BASE`, `_SIZE`, `_PAGE_SIZE` | from the MCU | Flash geometry |
| `LINBOOT_RAM_BASE`, `_SIZE` | from the MCU | RAM |
| `LINBOOT_BOOTLOADER_SIZE` | 0x1000 | Flash reserved for the bootloader (multiple of the page size) |
| `LINBOOT_META_ADDR` | last page | Metadata page: the last page, or the page right after the bootloader |
| `LINBOOT_BLOCK_SIZE` | page size (max 1024) | RAM write buffer; divides the page size |
| `LINBOOT_VERIFY_APP_ON_BOOT` | ON | Check the application CRC on every boot |
| `LINBOOT_STACK_SIZE` | 0x400 | Minimum stack, checked by the linker |
| `LINBOOT_MINIMAL_LIBC` | ON | Small `memcpy`/`memset`/`memmove` |
| `LINBOOT_LTO` | ON | Link time optimization |
| `LINBOOT_BUILD_EXAMPLES` | ON | Build `examples/` |

STM32F0 port options:

| Option | Default | Description |
|---|---|---|
| `LINBOOT_CLOCK` | HSI48 | HSI48, HSI (8 MHz) or HSE (used directly) |
| `LINBOOT_HSE_HZ`, `LINBOOT_HSE_BYPASS` | 8000000, OFF | HSE frequency, external clock instead of a crystal |
| `LINBOOT_LIN_USART` | 1 | USART used for LIN (must support LIN mode) |
| `LINBOOT_LIN_TX_PIN`, `_RX_PIN`, `_AF` | PB6, PB7, 0 | LIN pins and their alternate function |
| `LINBOOT_LIN_EN_PIN`, `LINBOOT_LIN_EN_ACTIVE_LOW` | (none), OFF | Optional transceiver enable pin |
| `LINBOOT_STM32F0_FORCE_FLASH_BOOT` | OFF | Program the option bytes to ignore the BOOT0 pin (for boards where it floats, like the USB LIN Card) |

**Clock accuracy:** a LIN commander needs a clock within ±0.5 %. The F042's HSI48 is only that
accurate when trimmed by the CRS from USB, which the bootloader doesn't use (it runs about 0.6 %
fast untrimmed). That is fine on the bench, but a production COMMANDER role device should use an
HSE. Responders tolerate more (±1.5 %).

# Integration guide

## Hooks

The hooks are declared in `src/hooks/linboot_hooks.h`, with weak defaults. Override them in a source
file given in `LINBOOT_EXTRA_SOURCES`:

- `uint16_t linboot_hook_hardware_version(void)`: the hardware version of the board.
- `void linboot_hook_board_init(void)`: board setup after the clocks (e.g. transceiver power).
- `void linboot_hook_before_boot(void)`: undo the board setup before starting the application.

## Application requirements

An application for the bootloader is a normal application with a different linker script. The
build generates a reference script for the configured layout: `build/<preset>/generated/app.ld`.

- **Flash:** the application starts at the application address (0x08001000 by default), and
  must not reach the metadata page (the last page by default).
- **RAM (Cortex-M0 only):** the bootloader copies the application's vector table (first 0xC0
  bytes) to the start of the RAM and remaps the RAM to address 0 (`SYSCFG_CFGR1.MEM_MODE`), as the
  M0 has no VTOR. The application must not use the first 0xC0 bytes of the RAM, and must not
  change `MEM_MODE`. (A port for a core with VTOR would set `SCB->VTOR` instead and reserve no RAM.)
- **Starting state:** like after a reset. The clock is HSI, the peripherals the bootloader used
  are reset, interrupts are enabled but nothing in the NVIC is, and the stack pointer comes from the
  application's vector table.
- **Image:** the `.bin` of the application (`objcopy -O binary`), and a version for it.

For an STM32CubeMX generated application, change the `MEMORY` of the linker script, e.g. for the
F042 with the default layout:

```
RAM (xrw)  : ORIGIN = 0x200000C0, LENGTH = 6K - 0xC0
FLASH (rx) : ORIGIN = 0x08001000, LENGTH = 27K
```

`examples/blinky` is a minimal application: it blinks an LED from the SysTick interrupt (which also
shows that the vector table relocation works), at 1 Hz (`blinky-slow`) or 5 Hz (`blinky-fast`).
`blinky-reboot` blinks fast and resets the device after 3 s, so that a device without any other
way to reset it keeps coming back to its bootloader (used for testing).
The LED pin is `BLINKY_LED_PIN` (PA7 on the USB LIN Card).

# Developer notes

## Architecture

```
src/core/      platform independent, unit tested on the host
  linboot.c      main loop, minlin glue (frame table and callbacks)
  responder.c    RESPONDER role state machine
  commander.c    COMMANDER role state machine (bus transactions, provider selection)
  image.c        image writer: chunks -> RAM block buffer -> flash
  meta.c         metadata record
  crc32.c        CRC-32 (nibble table)
  proto.h        frame layouts and codes
  mini_libc.c    small memcpy/memset/memmove
src/port/port.h  port interface
src/port/stm32f0/  STM32F0 port (startup and vector table, clock, LIN USART, flash, jump)
src/hooks/       integrator hooks and their weak defaults
src/linboot_config.h.in   configuration header, generated by CMake
cmake/           toolchain, options, port options, linker script template
examples/blinky/ example application
tools/linboot/   host tool (Python)
test/            host unit tests (Unity), simulated flash and bus
```

- **No HAL:** register access with the CMSIS device headers keeps the bootloader small and the
  options (pins, clock, layout) easy to set from CMake.
- **Flash stalls:** the CPU stalls while the flash is erased or programmed (the code runs from the
  flash), so nothing is received meanwhile; the protocols are built around that (the RESPONDER
  window and busy STATUS, the COMMANDER pausing the bus). USART overrun detection is disabled, so
  that reception continues after a stall.
- **Echo:** the transceiver echoes everything back. The COMMANDER role sends headers directly and
  publishes its own QUERY response through minlin's responder, which sees the echoed header.
- **Size:** LTO and the minimal libc keep the bootloader at about 3.5 to 4 KB; the remaining big
  parts are minlin, the flash driver and the option byte code.

## Porting

A port consists of `cmake/ports/<name>.cmake` and `src/port/<name>/`:

- The CMake file sets the `LINBOOT_PORT_*` defaults: flash and RAM geometry, the flash write
  granularity (`LINBOOT_PORT_FLASH_WRITE_ALIGN`, e.g. 8 on G0/L4), the RAM reserved for the vector
  table (0 with VTOR), and the CPU flags. It also declares the port's options, fills in
  `LINBOOT_PORT_CONFIG` (the port part of `linboot_config.h`), and provides
  `linboot_port_setup_target()`.
- The sources implement `src/port/port.h` and the `minlin_hw_*()` functions of minlin, the startup
  code with the vector table, and feed received LIN bytes to `linboot_lin_rx_byte()`.

The STM32F0 port is register level; the USART LIN code is close to what other STM32 families with
the same USART (G0, L4, F3, ...) need, while the flash, clock and jump parts differ per family.

## Unit tests

```bash
cd test
cmake --preset test && cmake --build --preset test && ctest --preset test
cmake --preset coverage && cmake --build --preset coverage   # coverage report
```

The core is built for the host with a test configuration (`test/stubs/linboot_config.h`: small
pages and blocks; the geometry dependent tests also run with the block size equal to the page size
and 64-bit flash writes, as `*_page_blocks`), the simulated flash (`test/stubs/fake_flash.c`, with STM32F0 like programming
rules) and a simulated bus. The role tests play the other side of the bus, including lost frames,
slow providers, flash failures and timeouts.

The ctest run also runs the host tool's tests (`tools/linboot/tests`, pytest). Part of them run the
tool against the real role state machines, built as a shared library (`test/sim/linboot_sim.c`).

## Formatting and spelling

`./run-clang-format.sh` (clang-format 19) and `typos`.

## System tests

`test_sys/` has automated tests on real hardware (pytest), with two USB LIN Cards on the same LIN
bus (with the bus supply):

- **The device under test (DUT):** a USB LIN Card with an STLINK on its SWD header. The tests
  flash the bootloader on it (erasing everything), reset it, and check its state over SWD with
  STM32_Programmer_CLI: the metadata record, the application flash (read back), `SYSCFG_CFGR1`
  (`MEM_MODE` = 3 once the application runs), and the LED pin (blinky toggles it).
- **The adapter:** a USB LIN Card, used through the host tool's modules (and its CLI in one test
  per role). With card firmware 6 or later, `test_large_image` also checks the transfer speed.

The tests build the `usb-lin-card-{responder,commander}-release` presets themselves. If the DUT's
option bytes don't boot from the flash, the tests set nBOOT0=1 and BOOT_SEL=0. Each test sets up its
starting state over SWD (installing an application with its metadata, or erasing it), so the tests
don't depend on each other.

```bash
cd test_sys
pytest                     # everything, about 4 min
pytest -m "not slow"       # without the 20 KB transfers, about 2 min
pytest --adapter=/dev/ttyACM1 --programmer=/path/to/STM32_Programmer_CLI --no-build
```

The adapter port and the programmer can also be given with `LINBOOT_ADAPTER` and
`STM32_PROGRAMMER_CLI`; the defaults are the adapter used so far and
`~/STM32CubeProg/bin/STM32_Programmer_CLI`.

Covered in both roles: first install without an application (and that the bootloader keeps
waiting or looking), update in the boot window after a reset, same version (boots right away) and
FORCE, booting after the boot timeout, the device type check, an interrupted transfer (the device
stays in the bootloader, and the next update completes), a 20 KB image (marked slow), and the CLI.
The RESPONDER role also installs an older version, and the COMMANDER role chooses the highest
version among two providers (both played by the adapter). With a second adapter
(`--adapter2`/`LINBOOT_ADAPTER2`; skipped without one), `test_commander_multi.py` has two real
providers: the choice by version, index and FORCE, and falling back to the other provider when the
chosen one disappears mid-transfer or serves a corrupt image.

`test_responder_multi.py` has two RESPONDER DUTs on the bus (skipped if the second one doesn't
answer), updated with `update_nodes()` and the `update-nodes` CLI: both updated, one of them up to
date, and (slow) the waiting node kept in its bootloader during a 40 s update of the other one
(checked over SWD on the first DUT). The second DUT has no SWD:

- It's a USB LIN Card flashed with the bootloader of the `usb-lin-card-responder-node2-release`
  preset (IDs 0x20..0x22) over USB DFU: send `boot` to the card's serial port, then
  `STM32_Programmer_CLI -c port=usb1 -e all -w build/usb-lin-card-responder-node2-release/linboot.elf -v -g`.
  Entering DFU left the option bytes booting DFU; the bootloader fixes them when it first runs
  (`LINBOOT_STM32F0_FORCE_FLASH_BOOT`).
- It can't be reset, so the tests always install `blinky-reboot` on it, and check its firmware
  version from its STATUS when it comes back to its bootloader.
- To make it a USB LIN Card again, flash the usb-lin-card-fw over SWD.

Useful manual SWD commands: `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -hardRst` (reset),
`-r32 0x08007C00 24` (metadata), `-r32 0x40010000 4` (`MEM_MODE`).
