# Open-GBFlash

Replacement firmware for the **GBFlash** cartridge reader/writer (WCH CH579M).
It speaks the same cartridge protocol as the stock firmware and works with
[FlashGBX](https://github.com/Lesserkuma/FlashGBX), which needs one of its files
replaced so that it recognises the device. It is faster, in places by a lot.

Every dump this firmware produces is byte for byte identical to one taken with
the stock firmware. Nothing about how your cartridges are read or written
changes.

## How much faster

macOS, GBFlash v1.3, measured through FlashGBX against the same cartridges.
"Stock" is the official L15 firmware.

| Operation | Stock L15 | Open-GBFlash | |
|---|---|---|---|
| GBA, read 32 MiB (Stream) | 99.58 s (329 KiB/s) | **36.40 s (900 KiB/s)** | 2.7x |
| GBA, read 32 MiB (Single) | 103.87 s (315 KiB/s) ‡ | **42.02 s (780 KiB/s)** | 2.5x |
| GBA, write 16 MiB | 237.53 s (69 KiB/s) | **152.22 s (108 KiB/s)** | 1.6x |
| GBA Video (3D Memory), read 64 MiB | 271.10 s (242 KiB/s) | **78.40 s (836 KiB/s)** | 3.5x |
| Game Boy, read 2 MiB | 7.41 s (277 KiB/s) | **2.92 s (702 KiB/s)** | 2.5x |
| Game Boy, write 2 MiB | 33.16 s (62 KiB/s) | 32.43 s (63 KiB/s) | 1.0x |

Reads are the best of three per firmware, and every dump of a cartridge came
back byte-identical whichever firmware produced it. Writes were verified, best
of three, and wrote the same file over the same starting contents.

Stock is measured through unmodified FlashGBX, which is what a stock user
actually has. That matters: this project's FlashGBX patch negotiates a larger
read buffer, which is worth nothing on GBA and, on macOS, costs stock 22% on
Game Boy, so measuring stock through it would have flattered these numbers. On
Windows that penalty does not appear.

‡ Except this one. Only the Stream method was timed on unmodified FlashGBX, so
the Single row's stock figure comes from the patched host. Stock reads about 3%
slower through the patched host, so that row flatters this firmware slightly:
2.5x where the unmodified host would put it nearer 2.4x.

### The same cartridges on Windows

Windows 11, 32 cores, same device, same cartridges, same harness.

| Operation | Stock L15 | Open-GBFlash | |
|---|---|---|---|
| GBA, read 32 MiB (Stream) | 47.47 s (690 KiB/s) | **36.62 s (895 KiB/s)** | 1.3x |
| GBA, read 32 MiB (Single) | 78.30 s (418 KiB/s) ‡ | **42.15 s (777 KiB/s)** | 1.9x |
| GBA, write 16 MiB | 197.55 s (83 KiB/s) | **153.72 s (107 KiB/s)** | 1.3x |
| GBA Video (3D Memory), read 64 MiB | 137.75 s (476 KiB/s) | **80.60 s (813 KiB/s)** | 1.7x |
| Game Boy, read 2 MiB | 4.73 s (433 KiB/s) | **3.48 s (588 KiB/s)** | 1.4x |
| Game Boy, write 2 MiB | **30.09 s (68 KiB/s)** | 33.00 s (62 KiB/s) | 0.9x |

Every row is the same work as its macOS counterpart: same cartridges, same
files, same dumps byte for byte.

**Why the gains are smaller here, and it is not that this firmware is worse.**
Put the two GBA runs side by side, identical 32 MiB reads on both machines:

| | stock, macOS / Windows | Open-GBFlash, macOS / Windows |
|---|---|---|
| read (Stream) | 99.58 s / 47.47 s | 36.40 s / 36.62 s |
| read (Single) | 103.87 s / 78.30 s | 42.02 s / 42.15 s |
| write 16 MiB | 237.53 s / 197.55 s | 152.22 s / 153.72 s |

This firmware lands within 1% of itself on both machines. Stock is 1.2x to 2.1x
faster on Windows than on macOS. What limits this firmware is the USB link and
the cartridge, which no computer can help with; what limits stock is how quickly
the host turns commands around, and Windows is much better at that. Windows
users start from a higher baseline, so they have less to gain.

On macOS this firmware is worth 2.5x to 3.5x on reads. On Windows, 1.3x to
1.9x. Quote whichever matches the machine you use.

**One row goes the other way.** Game Boy writes on Windows are 10% slower on
this firmware than on stock. That path is bound by the cartridge's flash chip
rather than by anything the firmware does, so there was no headroom to win, and
on this host the small differences land against it. Measured over three runs,
and not explained.

Reads gain the most. Writes gain less, and on Game Boy they do not gain at all:
most of a write is the cartridge's own flash chip waiting to finish programming,
and no firmware can hurry that. The GBA write path still had firmware overhead
worth removing; the Game Boy one is already chip-bound at about 16 us a byte.

Two things worth knowing about the table:

- **Stream is the fastest read method.** FlashGBX offers Single, MemCpy and
  Stream for GBA. On stock the choice is worth about 6%; here it is worth 18%,
  because the cartridge side got fast enough for the difference to show. Stream
  is what the FlashGBX CLI already uses. In the GUI it is worth selecting.
- **Both firmwares wrote the same file over the same starting contents.**
  FlashGBX skips blocks that already match and blocks that are all 0xFF, so a
  write benchmark that is not set up carefully measures the skipping instead.

Take your own numbers with `tools/bench/`: it flashes both firmwares in turn and
times them on your machine, best of three, checking every dump against the
others.

### A note on board-to-board differences

Some GBFlash-compatible boards are intentionally performance-limited by the
original firmware when they do not satisfy its registration checks. Such boards
have been observed at approximately 87.5 KiB/s. Open-GBFlash does not implement
hardware registration or artificial performance restrictions. This build has
been tested on both official and clone GBFlash devices and there is no
measurable difference in performance or functionality.

If your board is one of the limited ones, the improvement you see will be far
larger than the tables above. Those were taken on a board the vendor firmware
runs at its normal speed.

## What actually changed

Same cartridge protocol, same commands, same dumps. The differences are
underneath.

- **It stops pretending to be a CH340.** The chip in a GBFlash is a WCH CH579.
  The stock firmware makes it enumerate as a CH340 USB-to-serial bridge, which
  is a different part, so every computer loads a driver written for hardware
  that is not there. Open-GBFlash uses its own USB identity and the standard
  serial class, so Windows, macOS and Linux all use the serial driver they
  already ship.
- **That is what allows full-size USB packets.** A real CH340 moves 32 bytes per
  USB transaction, and the driver bound to that identity expects 32. The CH579
  can do 64. An early build declared 64 while still claiming to be a CH340:
  macOS did not care, Windows enumerated the device and then hung on the first
  cartridge read. Taking a USB identity of its own is what made the bigger
  packets usable, and they are worth roughly double the transport bandwidth.
- **The processor runs at 40 MHz instead of 32.** That is the top of the CH579's
  rated range, fed from a clock source the vendor's own code does not pair with
  it. Faster was tried. The board stopped booting and needed recovering by hand.
- **Cartridge waits are measured instead of inherited.** Stock uses a handful of
  round numbers throughout. Every wait here was swept against a real cartridge
  down to where the data is still byte-perfect, then checked against a full dump
  taken with the stock firmware. Several came out shorter than stock's. Several
  came out longer: the GBA write setup window and a number of Game Boy save and
  bank-switching waits were too short, and now give the cartridge at least as
  long as the official firmware does.
- **Cartridge data goes straight out to USB.** Stock copies every byte into a
  staging buffer on the way. Dropping that trip also freed the memory that made
  larger transfers possible.
- **The host asks how much the device can send.** FlashGBX assumed 4 KiB. It now
  asks and gets 20 KiB, so the same dump costs far fewer round trips.
- **An endpoint nobody answers stops being polled.** Stock declares an interrupt
  endpoint that the host asks a thousand times a second and that never replies.
  Every one of those polls was bus time taken from the endpoint carrying your
  cartridge data.
- **Game Boy writing follows the right routine.** This firmware had been modelled
  on the wrong part of the stock write path, which left Game Boy ROM writes
  slower than stock. They are level with it now, without shortening any pulse the
  flash chip sees.
- **GBA Video cartridges stop staging every page through memory.** That path kept
  the copy long after the ordinary read paths lost it, and it is most of why
  those cartridges dump several times faster.
- **No registration check and no speed limiting.** Neither exists in this
  firmware. There is nothing to satisfy and nothing to pass.

If a cartridge ever reads wrong, every one of these timings is a build-time
setting with the safe direction written next to it, so a more conservative build
is a recompile away.

## The included hw_GBFlash.py

FlashGBX needs one of its files replaced. Not a patch you apply: this repository
ships a finished `hw_GBFlash.py` at the root of this repository, which you copy
over `FlashGBX/hw_GBFlash.py`, replacing the file that is there. Keep the
original if you want to go back.

Four things differ from the file it replaces:

- **It recognises the device.** FlashGBX finds a GBFlash by USB id, and looks
  only for the CH340 that the stock firmware pretends to be. Open-GBFlash has
  its own id, so without this it is simply never found.
- **It asks the device how much it can send at once**, instead of assuming
  4 KiB. Open-GBFlash can do more, and reading in larger pieces means fewer
  round trips over USB.
- **It recovers a device that stopped answering.** If a transfer is interrupted,
  the firmware can be left waiting for the rest of a command, and everything
  sent afterwards gets swallowed as arguments. The device then looks absent.
  This clears the line and tries again before giving up.
- **It finds the bootloader again after the identity changes.** Handing over
  from firmware to bootloader changes the USB id, so the port the updater was
  talking to disappears. This looks the bootloader up rather than assuming the
  name stayed the same. It does not remove the U22 step: FlashGBX's updater
  finds the device with its own port scan first, in a file this does not
  replace, and that scan only looks for the stock id.

None of it changes how cartridges are read or written. The same file works with
the stock firmware, so there is no need to swap it back to use stock. One
caveat if you do: on macOS the stock firmware reads Game Boy cartridges about
20% slower through this file than through the original, because it negotiates a
larger read buffer that the stock firmware gains nothing from. On Windows there
is no difference.

## Installing

You need a GBFlash v1.0 to v1.3 and Python 3. Only v1.3 has been tested.

1. Download `fw_GBFlash.zip` from the releases page.
2. Replace the copy inside FlashGBX: `FlashGBX/res/fw_GBFlash.zip`.
   Also copy `hw_GBFlash.py` from this repository over
   `FlashGBX/hw_GBFlash.py`.
3. In FlashGBX, choose **Tools > Firmware Updater**, or from a terminal:

   ```
   python3 run.py --cli --action fwupdate-gbflash
   ```

4. Unplug the device, hold the small **U22** button on the board, plug it back
   in while still holding. The blue **ACT** LED blinks twice, repeatedly.
5. Release the button and let the updater finish.

**If the ACT LED does not blink twice repeatedly, the board probably has no
working bootloader.** Clone boards are often shipped without one, so that they
cannot be updated: an update would restore the vendor firmware's "cloned
hardware" warning, which the shipped image has had patched out. (Curiously, many
of them leave the registration speed limiting in place.)

That is only a problem for installing firmware, and it is fixable. There is an
open source bootloader that accepts updates:
<https://github.com/Daemon125/Open-GBFlash-Bootloader>. Install that first, then
come back to step 1.

Once you are on Open-GBFlash this stops being a concern entirely. There is no
cloned-hardware warning, no registration check, and no artificial speed limit
applied according to which AliExpress seller you happened to buy from. Your
board runs as fast as your board can run.

## Building from source

The LK firmware skeleton this is built on is a submodule, so it needs a
recursive clone. Without it the build stops on missing headers.

```
git clone --recursive https://github.com/Daemon125/Open-GBFlash-Firmware
```

If you already cloned without it:

```
git submodule update --init --recursive
```

Then, with `arm-none-eabi-gcc` on your path. Releases are built with Arm GNU
Toolchain 15.3.Rel1 (gcc 15.3.1), which is the toolchain the firmware was tested
with; another version will compile but may not produce the same image:

```
make            # build/fw.bin, the installable image
make dist       # dist/fw_GBFlash.zip, what FlashGBX installs
./tools/flash.sh
```

## Going back to the official firmware

The stock firmware ships inside FlashGBX, so you always have a copy.

1. Restore FlashGBX's own `FlashGBX/res/fw_GBFlash.zip` (reinstall FlashGBX, or
   keep a backup of the original before step 2 above).
2. Run the firmware updater again, holding **U22** as before.

**Holding U22 is required going back, not optional.** The bootloader still
presents itself as a CH340, the same as the stock firmware. When Open-GBFlash
hands over, its own USB id disappears, and FlashGBX reads that as the update
having failed: it does not go back and look for the stock id. Putting the board
into update mode by hand with U22 sidesteps the whole handover.

The bootloader is never touched by any of this, so the U22 recovery path always
works even if a firmware image is bad. If the device stops enumerating entirely,
hold U22 while plugging in and reflash: that is the bootloader, not the
firmware, and it cannot be bricked by a bad update.

## Compatibility

Tested on GBFlash v1.3, on both an approved board and a clone board, with no
difference in speed or behaviour between them.

Cartridges exercised:

- **Game Boy Advance**: ChisFlash 16 MiB flash cartridge with a 1M FLASH save,
  Pokemon Emerald repro, Super Mario Advance 4, Dragon Ball Z: The Legacy of
  Goku, and a Shark Tale GBA Video (3D Memory) cartridge. Super Mario Advance 4
  is a genuine mask ROM, read on all three methods and on both firmwares to the
  same md5, and checked against FlashGBX's own ROM database
- **Game Boy**: ChisFlash MBC3 2 MiB flash cartridge, a generic AliExpress
  flash cartridge, Pokemon Yellow repro, Pokemon Gold, Casper, and Rugrats
  Time Travelers, the last of these a genuine mask ROM read six times across
  both firmwares to the same md5

Every read was compared byte for byte against a dump taken with the stock
firmware, and every write was verified.

### Not tested

The cartridge protocol and the bus waveforms are the stock ones, so these should
behave as they always did. They have simply never been in front of this
firmware:

- **Board revisions other than v1.3.** The installer accepts v1.0 to v1.3, but
  only v1.3 has been tested.
- **MBC1 and MBC2**, the two commonest Game Boy mappers, along with MBC6, MBC7,
  MMM01, HuC-1, HuC-3, TAMA5 and the unlicensed mappers. Only MBC3 and MBC5 have
  been used.
- **Game Boy flash cartridges that program through the audio pin** rather than
  the normal write pin.
- **Mapper-specific save paths** for chips like the one in Kirby Tilt 'n'
  Tumble, and the boot handshake some cartridges need. That code is transcribed
  from the stock firmware and has never run against the hardware it exists for.
- **Nintendo Power GB Memory cartridges**, Game Boy Camera, and cartridges with
  rumble or an accelerometer.
- **Linux**, with a cartridge. It should be fine, since the device is a standard
  serial port, but nobody has dumped anything on it.
- **GBA Video cartridges other than Shark Tale.** That whole path rests on one
  cartridge, and it is the one path where a wait too short returns a dump that
  looks perfectly valid and is not.

## Warranty, and a word about your cartridges

**No warranty. Use at your own risk.** This is unofficial firmware. Nobody
involved is responsible for damaged hardware, damaged cartridges, corrupted or
lost save data, or a cartridge you can no longer read.

**Check it against the stock firmware before you trust it with anything you
care about.** Dump a cartridge with Open-GBFlash, dump it again with the stock
firmware, and compare the two files. Do the same for a write. If they match,
your hardware is fine on this firmware. That takes a few minutes and it is the
only way to know for certain on a cartridge nobody has tried.

This matters more than it usually would, for a specific reason. The timings
here were measured on real hardware and taken down to where the data is still
byte-perfect, rather than left at the comfortable margins the stock firmware
uses. Boards vary. Most clones are indistinguishable from an approved board
apart from the X1 crystal, which makes no difference, but some vendors cut
corners in ways that only show up when the hardware is actually being pushed. A
board that seems perfectly happy on the stock firmware may have faults that the
stock firmware's slower pace, or its artificial speed limiting, was hiding.

If something does read wrong, the timings are all build-time settings with the
safe direction documented next to each one, so a slower build is a
recompile away.

## Licence

GPL v3, the same as the LK firmware skeleton this is built on and the same as
FlashGBX. See `LICENSE`.

## Thanks

Lesserkuma, for FlashGBX and the LK firmware skeleton this is built on.

Charlie SIGMA and smellyghost on Discord, who tested this on approved GBFlash
boards before there was one on this desk, and found a compatibility problem with
the stock bootloader that would not have turned up on a clone.
