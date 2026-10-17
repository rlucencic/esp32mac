# esp32mac: Macintosh Plus / SE / Classic on the Makerfabs MaTouch ESP32-S3 7"

A compact black-and-white Mac (68000, 512x342, up to 4 MB) on an ESP32-S3
board with a 1024x600 touch LCD. Bluetooth keyboard and mouse, the touch
screen as a mouse, disk images of any size in three drives, an LVGL setup
screen, and a web page for disk images and live status. ESP-IDF v5.5.4, C.

The board side (tasks, LCD, touch, SD, WiFi, BLE, setup screen, web page,
console) and the 68000 core are the shared library in `../esp32emu`, also
used by `../esp32ste` (Atari STE), `../esp32cpc`, `../esp32zx`, `../esp32cbm`
and `../esp32xl`. See its CLAUDE.md for the runtime and the board-level
lessons; changes there need all six projects built, for both boards.

## Hardware

Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7", ESP32-S3-WROOM-1-N16R8
(16 MB flash, 8 MB octal PSRAM), 1024x600 RGB565 panel, GT911 touch, microSD,
MAX98357A amplifier. Pins: `../esp32emu/components/emu/include/emu/board.h`.
The native USB port is the console, `/dev/cu.usbmodem1101` on the user's Mac
(was `3101` for a while).

## Build, flash, debug

Build like the ST (the shell's default Python environment doesn't match):

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py build && idf.py -p /dev/cu.usbmodem1101 app-flash'
```

- `app-flash` writes only the app; `idf.py flash` also writes `roms/plus.rom`,
  `se.rom`, `classic.rom` (not in git) to partitions `rom_plus`, `rom_se`,
  `rom_classic`. Those overlap the ST's TOS partitions: switching the board
  back to the ST needs a full `idf.py flash` there. NVS (WiFi, BLE pairings)
  is shared between the two firmwares.
- `../esp32emu/tools/emu_console.py PORT log N | screenshot F.png | lcd F.png | send "cmd" ... | info | reset`
  with the IDF venv's Python (`-I`). One touch per `send` call.
- `curl http://macintosh.local/api/status`; disks: see `../esp32emu/README.md`
  (`PUT /disks/<name>` upload, `PUT /api/drive/<n>` insert by name).
- Flashing restarts the board and ends the user's Mac session: say so first.

Console commands: the shared ones (`s` `L` `r` `F` `t` `i`, see
`emu/console.h`) and the Mac's (`main/main.c`): `k <hex>` tap a Mac key code,
`K <hex> 0|1` key down/up, `m dx dy btn` relative mouse, `p x y btn` pointer
at a screen position, `R <model 0|1|2> <ram KB>` restart as Plus/SE/Classic,
`n` interrupt switch.

## M5Stack Tab5

The same code also builds for the M5Stack Tab5 (ESP32-P4, target esp32p4),
beside the MaTouch build, on `/dev/cu.usbmodem3101`:

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig build && \
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig -p /dev/cu.usbmodem3101 flash'
```

- Its settings: this project's `sdkconfig.defaults`, then
  `../esp32emu/boards/m5stack_tab5/sdkconfig.defaults` (`CMakeLists.txt`),
  into `build-tab5/sdkconfig` (delete it and `reconfigure` after changing
  either). Board details and lessons: `../esp32emu/CLAUDE.md`, "The two
  boards".
- The Tab5 is shared by the projects like the MaTouch: after another
  project, a full `flash` (partition table and ROMs). The MaTouch build
  afterwards recompiles everything once (shared `managed_components/`).
- Measured on the Tab5 (2026-10-17): Mac Plus with 4 MB at the "?" disk (no image on that card): 98-100%, core 1 19% busy; the picture 1078x720, smooth. Sound on by default
  (ES8388); the console stays on USB.
- Not checked by eye yet: the picture on the panel itself (orientation),
  touch, sound, the Tab5 Keyboard's keys, Bluetooth pairing.

## Layout

| Path | What |
|---|---|
| `main/main.c` | The Mac as an `emu_machine_t`: memory, start, key map, touch, drives, status, console |
| `main/rom_store.c` | ROM from its flash partition, else a matching file in the SD card's `roms/` |
| `main/settings.c` | NVS `mac`: `model`, `ram_kb`, `pram` (256 bytes) |
| `main/web/macfiles.js` | Web page script (`/machine.js`): Mac archives -> HFS disk images in the browser (LGPL, see below) |
| `components/macintosh/` | The Mac, platform independent (`include/mac.h`) |
| `components/macintosh/host/` | Host build for testing on the Mac (see Testing) |
| `roms/` | `plus.rom`, `se.rom`, `classic.rom` (copyrighted, not distributed) |
| `partitions.csv` | nvs, phy, factory 8 MB, rom_plus 128K, rom_se 256K, rom_classic 512K (type 0x40), storage |

## The core (`components/macintosh`)

- RAM and ROM copy stored with each 16-bit word in host order (byte n at
  index n^1), like the ST. `mem.c`: memory map (overlay: ROM at 0 and RAM at
  $600000 after reset; Plus clears it with VIA PA4, SE/Classic on the first
  access to $40xxxx), fast paths for RAM and ROM.
- `mac.c`: time in CPU cycles (7.8336 MHz); the CPU runs to the next
  scheduled event (VIA T1/T2, keyboard and ADB replies) or frame landmark
  (VBL after line 342, frame end at 370 lines = 130240 cycles, 60.15 Hz);
  `mac_schedule()` ends Musashi's timeslice when an event comes sooner. VIA
  IRQ = level 1, SCC = 2, interrupt switch = 4, autovectored. RESET on a
  Plus restarts the whole Mac (the Finder's Restart relies on it), on the
  SE/Classic it resets the peripherals.
- `via.c`: 6522 with real timers (kept as start time + value), shift register
  for keyboard/ADB, IFR/IER, port wiring through `via_port_*_changed()`.
  CA1 = VBL, CA2 = one-second.
- `rtc.c`: RTC serial protocol on PB0-PB2, seconds from the host's local
  time (+ offset when the Mac sets the clock), 256 bytes of (X)PRAM. Blank
  PRAM gets Mini vMac's defaults (`mac_pram_defaults()`): see the lessons.
- `keyboard.c` (Plus): Inquiry/Instant/Model/Test commands; keypad and arrow
  keys answer $79 and come with the following Instant. `adb.c` (SE,
  Classic): transceiver states on PB4/PB5, keyboard at 2, mouse at 3,
  sequence as in Mini vMac.
- `input.c`: input queue from other tasks; the pointer through low memory
  (MTemp $828, RawMouse $82C, Mouse $830, CrsrNew $8CE): Plus relative motion
  and touch positions; ADB relative motion goes through the ADB mouse
  (acceleration applies). Button changes at most every 3 frames.
- `chips.c`: SCC (registers, no data/interrupts), IWM (mode/status), SCSI
  5380 (empty bus: arbitration works, selection times out).
- `video.c`: screen at RAM top - $5900 (alt. $D900, PA6), sound buffer at
  RAM top - $300 (alt. $5F00, PA3), one byte per line, volume PA0-2, PB7 off.
- `rom.c`: known ROMs by checksum (Plus v1-v3, SE, SE FDHD, Classic);
  patches checked against the expected bytes: checksum test, RAM test,
  and the .Sony driver (at $17D30 Plus, $34680 SE/Classic) replaced.
- `sony.c`: the replacement driver. 68000 stubs, hand-assembled in the
  source (verify with `MAC_DIS=<addr>:<n>` after changes), call native
  routines through ILLEGAL ($4AFC) + function word, honoured only inside the
  driver. Open allocates drive records with `_NewPtr`, adds drives 1-3 with
  `_AddDrive`, installs a VBL task (`_VInstall`) that posts disk-inserted
  events (`_PostEvent`). Prime reads/writes 512-byte blocks at the DCE
  position; Control: eject, verify, format (no-op), drive info; Status:
  drive status, format list (the image's size: Disk Init then offers
  "Macintosh 20 MB" etc.). Drive queue elements are qType 1 (32-bit size).
  Two driver headers share the code: `.Sony` for floppy-sized images (up to
  1.4 MB), `.Disk` for bigger ones (its DCE is made by the native Open in
  the last unit-table entry, marked open; diskInPlace 8 = fixed disk).
  CD images (`.iso`, `.toast`, `.cdr`) are opened read-only (locked).
  Fixed disks (over 1.4 MB) get a hard disk icon of the driver's own (an
  original drawing, ICN# data + mask copied into the ROM after the driver),
  returned by Control 21/22 (media/physical icon) — without it the Finder
  shows a floppy; Control 23 (drive info) says fixed or 3.5", internal or
  external. The 7.5 System has no ICN# -3995 to borrow.
  Images: raw, Disk Copy 4.2 (84-byte header), Apple partition map (first
  Apple_HFS partition).

## Mac archives on the web page (`main/web/macfiles.js`)

Embedded by `main` and handed to the runtime as `web_script` (served as
`/machine.js`); it calls `registerConverter()`, and the page then accepts
`.sit .sea .cpt .hqx .bin` (also inside ZIP files), converts them in the
browser and uploads the result as an ordinary image.

- Containers: StuffIt 1.5-4.5 (`SIT!`, 112-byte entries) and 5 (`StuffIt
  (c)1997-`, also found inside a self-extracting app's data fork), Compact
  Pro (directory CRC-checked), BinHex 4.0, MacBinary I/II/III.
- StuffIt methods 0 (stored), 1 (RLE 0x90), 2 (LZW, 14 bits), 3 (Huffman),
  5 (LZAH), 13 (LZSS+Huffman), 15 (Arsenic: arithmetic coding, BWT, MTF).
  Not: 8 (MW), 14, encrypted archives, StuffIt X (`.sitx`). Compact Pro:
  RLE and LZH. Every fork is checked against its CRC.
- The files then go onto a new HFS volume: the smallest of 800K, 1.4 MB, or
  a hard disk (MB-rounded, 25% spare), named after the archive. B-trees with
  512-byte nodes, catalog in Mac name order (machfs's table), forks in one
  extent each, Finder flags kept except "inited" (the Finder places the
  icons). Disk images found inside an archive (by extension, Disk Copy 4.2
  header, or `dImg`/`rohd` of a floppy size) are uploaded as they are.
  Archives it can't open (DiskDoubler, `.zip` inside) stay files, with room
  to expand them on the Mac.
- Follows The Unarchiver's XADMaster (LGPL), including its method 13 tables,
  so this one file is LGPL 2.1+; the rest of the project stays MIT.
- Test it under node: `module.exports` has `convert`, `open`, `buildHfs`
  (see Testing).

## Persistent state

- NVS `mac`: `model` (0 Plus, 1 SE, 2 Classic), `ram_kb`, `pram`.
- NVS `disks`: `disk_a`..`disk_c` (file names), `empty_n` (last BLANKnnnn).
- SD card: `disks/` (images, made if missing), `roms/` (optional ROMs).

## Memory budget (measured)

- PSRAM: opcode tables (320 KB), then Mac RAM (up to 4 MB), then the ROM
  buffer (512 KB, any model), first thing. `PSRAM_FOR_THE_REST` is 1.75 MB
  plus the ROM buffer (with 2 MB margin and the ROM buffer first, 4 MB
  didn't fit and the Mac got 2 MB). With 4 MB: ~500 KB PSRAM free while
  running, ~340 KB with the setup screen open.
- Internal RAM: ~58 KB free while running (33 KB in the first build, when
  the 16 KB sound ring was internal; now it's PSRAM, made only with the
  speaker on).

## Lessons

- **Blank parameter RAM hangs System 7 in AppleTalk** at the Finder: the
  printer port defaulted to AppleTalk, which waits for SCC transmit
  interrupts this emulation doesn't make. PRAM byte $13 = $22 (both ports
  async) fixes it; blank PRAM gets Mini vMac's defaults.
- **Disk-inserted events posted during start-up get flushed**: a disk in
  drive 2 or 3 at power-on never mounted. The VBL task now posts a disk
  again (every 2 s, up to 2 minutes) while nothing has read it since and no
  disk event for it waits in the Mac's event queue ($14A).
- **System 7.5.3's floppy install set refuses the Plus** ("This startup disk
  will not work on this Macintosh model") — genuine, not an emulation bug:
  the SE and Classic boot the same disks, and the Plus boots the 7.5.3 CD
  image's System.
- **Panel artefacts and a picture shifted by ~5 lines** on the first board
  build: the ROM was read with `esp_partition_read()` (caches off during the
  read; now memory-mapped), LVGL had resolved to 9.6, and the presenter
  rewrote the whole picture on every change (now only changed rows; see
  `../esp32emu/CLAUDE.md`). Those were only what set it off: the shift
  stayed because ESP-IDF's RGB driver never got back in step, came back on
  the ST (2026-10-17) and is fixed in `../esp32emu/components/esp_lcd`.
- **Free RAM** (web page): the Process Manager's heap zone, found right
  after the system heap (`mac_ram_free()`), else the application heap.
- **The 7.5.3 Installer ejected the CD image and asked for "System 7.5.3
  Install"**: it treats any volume on `.Sony` as a floppy set, where the next
  "disk" must be a volume of that name. On a hard disk or CD it uses the
  folder of that name next to it. Hence the `.Disk` driver for images over
  1.4 MB. Installing 7.5.3 from the CD image onto a 500 MB image then
  completed, and the Plus boots it.
- **SonyVars ($134) must not point at the driver's data**: system code
  writes into the real Sony driver's variables, and it overwrote the first
  drive queue element ("Memory" control panel: address error at start-up,
  walking the drive queue). It points at a 1 KB scratch area now.
- **Unit table**: 48 entries on the Plus, 64 on the SE/Classic; units 32-39
  are the SCSI drivers' (system code looks into those), so `.Disk` takes
  the last entry.
- The first click after the Finder appears can be ignored while it's still
  starting up; that's the Mac, not the input path (MBState changes).
- The host build runs ~200x real time: boot tests take seconds.
- **Mac archive pitfalls** (`macfiles.js`): BinHex's CRC is the
  "augmented" CCITT (don't append two zero bytes to a table-driven CRC);
  Compact Pro's LZH offset 0 means 8192 (the 8 KB window wraps); in LZAH's
  adaptive tree, decide which child each swapped node is before changing
  either parent (siblings); HFS index nodes need sibling links too (Disk
  First Aid: "Invalid sibling link").

## Testing

- **Host build**: `make -C components/macintosh/host`, then
  `build/mac_host <rom> [frames] [out.ppm] [ram_kb]`. Environment:
  `MAC_DISK1..3=<image>`, `MAC_PRAM=<file>`, `MAC_SCRIPT=<file>` (lines
  `<frame> key <hex> 0|1 | type <text> | mouse dx dy b | pos x y b |
  shot <file.ppm> | insert <d> <path> | eject <d> | peek <hex addr>`),
  `MAC_STATUS=<n>`, `MAC_TRACE=<pc>[:<n>]`, `MAC_DIS=<addr>[:<n>]`,
  `MAC_FAULT_DIS` (code, registers and drive queue at a bus/address error;
  the errors themselves are always logged in the host build), script
  commands `free`, `units`, `icon <hex addr>` (prints a 32x32 bitmap),
  `MAC_WAV=<file>`, `MAC_QUIET`. Prints blocks read/written per drive.
- Verified on the host: Plus boots 7.5.3 from the CD image (partition map),
  keyboard and mouse; SE and Classic boot the Disk Tools floppy to the
  Finder; ADB keyboard (Cmd-N, typing) and mouse (menus); absolute pointer;
  blank 20 MB disk initialised and mounted; three drives at power-on;
  eject from the Mac; startup sound.
- **Mac archives**: `node` with `require("main/web/macfiles.js")`; 73 of 74
  Info-Mac files (BinHex, MacBinary, StuffIt 1.5-5, Compact Pro, methods
  0/2/3/5/13/15) converted with all CRCs right (the 74th is a truncated
  download); every image read back with machfs (`pip install machfs`); Disk
  First Aid (7.5.3 Disk Tools) finds a 52 MB, 300-file, 3-level-catalog
  volume and the Speedometer floppy OK; Speedometer 3.23 runs from its
  converted `.sit`. Method 1 (RLE) only by reading. Old archives:
  `ftp.funet.fi/pub/mac/` (`info-mac/`, `cdev/`, `games/`...).
- On the board: Plus (4 MB) boots 7.5.3 from the CD image and SE from Disk
  Tools at 100% speed (60 fps; dips to ~65-90% while blocks load from the
  SD card, in the emulation task); touch clicks; web upload of the 255 MB
  CD image (~400 KB/s) while the page stays responsive; insert by name
  through the API. Speedometer's `.sit` dropped on the board's page,
  inserted and run (Benchmark Mix: 1.3 x a Mac Classic, Plus at 4 MB).

## Open items

- No SCC serial/AppleTalk, no real IWM (copy-protected floppies), no SCSI
  devices, no Mac II/colour.
- Classic: its ROM disk (Cmd-Option-X-O held at power-on) doesn't start in
  the host build; not investigated (key timing through ADB, or the ROM
  disk's own checks).
- Disk reads run in the emulation task: big reads cost frames.
- System 6 not tested.

## README and GitHub

Public at `github.com/rlucencic/esp32mac` (pushed to `main`), with topics
and an About text like esp32ste's. README images in `docs/images/` (how
they're made: `../esp32emu/CLAUDE.md`, README screenshots):

| Image | What |
|---|---|
| `desktop.png` | Mac screen (`screenshot`): System 7.5.5 on the 500 MB "Macintosh HD" image |
| `speedometer-test.png`, `-mix.png`, `-compare.png` | Speedometer 3.23 on the board (from its converted `.sit`): Performance Test, Benchmark Mix (1.298 x a Classic), comparison with an LC III |
| `desktop-panel.png` | The whole panel (`lcd`) |
| `setup.png` | Setup screen before the start, WiFi/time lines cut out (1024x514) |
| `disks.png` | Setup screen's Disks page |
| `web.png` | Web page with example data, after dropping the Speedometer `.sit` |

## Working with the user

- Expects changes built, flashed and checked on the board, then a short
  report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing. Screenshots: a new file each time,
  never over an existing one; the user often adds them to the README.
