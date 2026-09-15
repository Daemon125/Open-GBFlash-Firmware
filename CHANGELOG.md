# Changelog

## 1.2.0

**A dump could come back corrupted and be reported as complete.** One in fifteen
16 MiB dumps shifted by a byte partway through. If you have dumps taken with
1.0.x or 1.1.0 that you have not checksummed, re-dump them. Every read path is
also faster.

macOS, against the figures 1.1.0 published:

| Operation | 1.1.0 | 1.2.0 |
|---|---|---|
| GBA, read 32 MiB (Stream) | 900 KiB/s | **941 KiB/s** |
| GBA, read 32 MiB (Single) | 780 KiB/s | **830 KiB/s** |
| GBA, write 16 MiB | 108 KiB/s | **119 KiB/s** |
| Game Boy, read 2 MiB | 669 KiB/s | **720 KiB/s** |
| Game Boy, write 2 MiB | 80 KiB/s | **81 KiB/s** |
| GBA Video (3D Memory), read 64 MiB | 836 KiB/s | 814 KiB/s |

Both hosts, all six operations, stock against this firmware: see the README.
Windows numbers moved less, because stock starts from a higher baseline there.

### Upgrading from 1.1.0

Copy `hw_GBFlash.py` over `FlashGBX/hw_GBFlash.py` as well as installing the
firmware. The corruption fix is in that file, not in the firmware, so a device
updated through the Firmware Updater alone still produces shifted dumps.

### Fixed

- **A ROM dump could shift by one byte partway through and still be reported as
  complete.** `_try_write`'s resync writes `0x00` and reads the next byte as its
  answer. An ACK arriving between the flush and that read is taken instead, so
  the `0x00`'s ACK is read as the resent command's and the command's real ACK is
  left to be read as the first byte of the next bulk transfer. Seen once in
  fifteen 16 MiB dumps, on the stock read path as well as the pipelined one.
  FlashGBX reports the backup complete; only a ROM checksum in its database
  catches it. The fix quiets the port after the resync. This is upstream's code,
  so it affects every FlashGBX device family at `fw_ver` 12 and above.
- A GBA dump of a cartridge whose profile enables pull-ups dropped to the Single
  read method and stayed there for the rest of the dump.
- The 3D Memory read returned short on an odd trailing byte instead of padding
  it, desynchronising every command after it.
- The AGB read published on the host's step rather than the count the loop
  advances by, leaving a whole chunk unpublished when the one did not divide the
  other. Equal on every default.

### Faster

- **The host keeps one ROM read opcode outstanding.** Median 919.7 to 967.5
  KiB/s over interleaved 16 MiB AGB dumps. Engages at chunks of 0x1000 and
  above, so header, CFI and detection reads keep upstream's cadence.
- The ROM read's transfer size and access mode are sent once instead of before
  every chunk.
- **Two per-packet jobs left the USB interrupt, and EP2's transmit window is
  predicted rather than read back.** AGB Single 795.2 to 820.5 KiB/s, ahead in
  100% of pairwise comparisons: it is the one read path the cartridge leaf
  limits rather than the wire.
- **Save reads stopped re-setting two registers that nothing in the loop
  changes.** Worth 31% on AGB SRAM.
- A dead register reload left the AGB leaf's deselect, and its /RD-high fixed
  term came down from 12 cycles to 10.

### Changed

- `FW_USB_ISR_LEAN` and `FW_USB_TX_TOG_SHADOW` now default to 1.

### Measurements

- The 3D Memory row is 814 KiB/s on macOS and Windows alike, against the 836
  KiB/s published since 1.0.0. That figure came from a build that matches no
  released commit; every committed build measures 80.5 s on both hosts. Two
  interleaved rounds against the cartridge split between the USB interrupt knobs
  on and off, so they are not the difference.
- 1.0.0 said the Single row's patched-host stock figure flatters this firmware
  by about 3%. It does not. On the six rows both hosts can run, they
  agree on a stock device to within 1.06%, and the patched host is the slower of
  the two in five of them. The patched tree is needed for that row because
  upstream's CLI hardcodes Stream, not because it changes what a stock device
  does.
- The USB handler runs inside slack. It is not what limits read throughput.
- A save-read TX overlap knob corrupts the read. It ships off.
- A DMG publish grain of 64 is slower than what ships.
- A minimum chunk size on the pipelined read bought nothing.

### Tooling

- `tools/bench/` refuses to start without the read-method patch. Without it
  every AGB read row measures Stream and the Single row is a duplicate under a
  wrong label, with plausible numbers and matching dumps.
- `tools/flash.sh` rejects a bare path argument. It used to rebuild at default
  knobs instead, which has invalidated A/B measurements before.
- The host tests fail if an upstream method this project copied changes.

### Documentation

- The "Not tested" section claimed the bus waveforms are stock. They are not:
  the GBA /RD pulse ships at 11 cycles low against stock's 17, and /CS-high
  recovery at 36 against stock's 80.
- The host file is described as changing nothing about how cartridges are read.
  Three of its overrides now change exactly that when this firmware is attached.
- The dagger on the Single rows is gone. The patch it flagged is one line that
  lets the CLI select a read method; with the variable unset it makes upstream's
  own call.

## 1.1.0

**Game Boy ROM writes are 20% faster.** Every dump this firmware produces is
still byte for byte identical to one taken with the stock firmware.

| 2 MiB Game Boy write | time | rate |
|---|---|---|
| Stock L15 | 33.90 s | 60 KiB/s |
| Open-GBFlash 1.0.1 | 31.94 s | 64 KiB/s |
| **Open-GBFlash 1.1.0** | **25.66 s** | **80 KiB/s** |

One cartridge, one host, one FlashGBX, measured with `tools/bench/`. Both
firmwares wrote the same file over the same starting contents and every write
was verified byte-exact.

### Faster

- **Buffered writes take the lean bus primitive.** A 32-byte write-buffer load
  is 37 bus writes, and every one of them went through the heavy routine that
  re-arms the port direction and reloads the address. Worth 13.7% by itself.
- **Unlock bypass on the single-write path.** Two of the four bus writes per
  byte disappear. Worth 11.7% on a cartridge whose profile has no buffered
  write.
- **Programming starts before the block has finished arriving**, the Game Boy
  counterpart of the GBA pump.

### Changed

- The transfer buffer is 0x2000 instead of 0x5000. The larger size measured no
  faster and cost 12 KB of the 32 KB of SRAM; RAM use goes from 90% to 53%.

### Upgrading from 1.0.1

Nothing to do. Install it the way you installed 1.0.1.

## 1.0.1

Host-side fixes and a real build stamp. The cartridge paths are unchanged, so the
speed figures for 1.0.0 still stand.

### Upgrading from 1.0.0

1.0.0 said to replace `res/fw_GBFlash.zip` with this firmware. This release
expects that file to be the vendor's again: ours now goes in beside it as
`res/fw_Open-GBFlash.zip`.

If you followed the 1.0.0 steps, restore `res/fw_GBFlash.zip` by reinstalling
FlashGBX, then drop `fw_Open-GBFlash.zip` in next to it. Left as it is, the
updater shows no firmware choice, and the **Original firmware** button installs
this firmware rather than the vendor's.

### Fixed

- Switching back to the stock firmware failed on Windows unless the U22 button
  was held. The updater passed the port recorded before the handover, and the
  bootloader takes a different COM number, so opening it threw and the failure
  was reported as "No device found". The bootloader is now resolved by USB id
  before any port is opened. No button press is needed on Windows or macOS.
- A zip with a damaged `fw.bin` was offered by the firmware chooser and only
  failed once the device was already in the bootloader. The image is now read
  when the zip is inspected.
- A malformed `fw.ini` in `res/fw_Open-GBFlash.zip` stopped the device being
  connected at all, and stopped the updater opening to install anything else.

### Added

- The Firmware Updater offers a choice between the original firmware and
  Open-GBFlash, each read from its own zip in `res/`. The chooser is Charlie
  SIGMA's design. Without `res/fw_Open-GBFlash.zip` the dialog is upstream's,
  unchanged.

### Changed

- The firmware reports its own build date instead of the stock one. FlashGBX
  compares the device against `res/fw_Open-GBFlash.zip` and offers an update only
  when that file is newer. `SkipOpenFirmwareUpdate = enabled` in `settings.ini`
  silences the prompt.
- FlashGBX's **About Open-GBFlash** window, the firmware chooser, the install
  confirmation and the update-failure dialog give this project's page. The
  GBFlash hardware page is still shown for hardware questions.

### Measurements

- The macOS Game Boy write row was re-measured, both firmwares through the same
  host in one run: 33.81 s stock against 33.02 s here. The 1.0.0 table gives
  33.16 s and 32.43 s for the same row, from a run whose host was not recorded.
  The ratio is unchanged. Every other figure stands.

### Documentation

- The install steps offered FlashGBX's command line updater as an alternative to
  the graphical one. It always writes `res/fw_GBFlash.zip`, so it installed the
  stock firmware. The graphical updater is the only route.
- The note that stock reads Game Boy cartridges 20% slower through this host file
  on macOS no longer applies: the larger read buffer is negotiated only for this
  firmware, and a stock device keeps FlashGBX's 4 KiB.

## 1.0.0

First release. Based on the LK firmware skeleton, speaking the stock GBFlash
protocol, verified against a GBFlash v1.3.

### Speed

Measured on macOS through FlashGBX, against the official L15 firmware. Stock
reads are timed on unmodified FlashGBX, which is what a stock user has, except
the Single row (‡), whose stock figure comes from the patched host and flatters
this firmware by about 3%. Windows figures are in the README.

| Operation | Stock L15 | This release | |
|---|---|---|---|
| GBA, read 32 MiB (Stream) | 99.58 s | 36.40 s | 2.7x |
| GBA, read 32 MiB (Single) | 103.87 s ‡ | 42.02 s | 2.5x |
| GBA, write 16 MiB | 237.53 s | 152.22 s | 1.6x |
| GBA Video (3D Memory), read 64 MiB | 271.10 s | 78.40 s | 3.5x |
| Game Boy, read 2 MiB | 7.41 s | 2.92 s | 2.5x |
| Game Boy, write 2 MiB | 33.16 s | 32.43 s | 1.0x |

Game Boy writes are unchanged (superseded in 1.1.0): that path is bound by the
cartridge's flash chip, not by the firmware.

Every dump is byte-for-byte identical to one taken with the stock firmware.

### What changed

- Reports fw_ver 15 instead of 12, and implements what 15 means: PING answers
  the host's challenge complemented, SET_VAR_STATE acknowledges, and
  GET_SWITCH_STATE answers. Below 14, FlashGBX refused outright every Game Boy
  flashcart that programs through the audio pin. That path is reachable now,
  and still untested on hardware.

- System clock raised from 32 MHz to 40 MHz. Cartridge timings are expressed in
  cycles and rescaled, so the cartridge sees the intervals it saw before.
- Cartridge access intervals swept on hardware to their floors, on both the DMG
  and AGB read and write paths, each gated byte-for-byte against a stock dump.
- 3D Memory pages go straight to USB instead of through a staging buffer, and
  the mapper settle was measured rather than inherited. It has a real
  electrical cliff between 160 and 168 iterations; the shipped value sits 1.56x
  above it.
- CDC notification endpoint polling interval raised from 1 ms to 16 ms. It is
  never serviced, and it was costing bandwidth every frame.
- Transfer size raised to 0x5000, made possible by shrinking the USB staging
  buffer once 3D Memory stopped needing it.
- DMG publish granularity reduced so data leaves the device sooner.

### Fixed

- FlashGBX offered a firmware update on every launch that would have put the
  vendor image back. The build timestamp did not match the one FlashGBX holds
  for this PCB version, and that test is an equality. The displayed build date
  now reads as stock L15's, which is the cost of matching it.

- The millisecond timebase was derived from a hardcoded 32 MHz. Every deadline
  in the firmware fired at 80% of its intended wall-clock time once the clock
  changed. Now derived from the configured system clock, including for clock
  rates that are not a power of two.

### Known limits

- 48 MHz does not boot. There is no flash wait-state or core-voltage control on
  this part to compensate, so 40 MHz is the ceiling.
- The default build presents its own USB identity (`1209:0008`) and needs one
  is not recognised by FlashGBX until you copy the `hw_GBFlash.py` at the root
  of this repository over `FlashGBX/hw_GBFlash.py`. That file is not inside
  `fw_GBFlash.zip`, which holds only `fw.bin` and `fw.ini`.
- Save reading and save restoring are unchanged. Restoring is bound by the
  save chip, about 46 us a byte. Reading is bound by the host: adding 0.39 s of
  device work to a 128 KiB backup moved it by 0.03 s.
