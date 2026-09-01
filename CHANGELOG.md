# Changelog

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

Game Boy writes are unchanged: that path is bound by the cartridge's flash chip,
not by the firmware.

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
