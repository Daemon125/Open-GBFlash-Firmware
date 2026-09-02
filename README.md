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
| Game Boy, write 2 MiB | 33.81 s (61 KiB/s) | 33.02 s (62 KiB/s) | 1.0x |

Reads are the best of three per firmware, and every dump of a cartridge came
back byte-identical whichever firmware produced it. Reads are the best of three.
Writes were verified and wrote the same file over the same starting contents,
timed once after an untimed priming write.

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
- **The host asks how much the device can send.** FlashGBX assumed 4 KiB. It
  asks this firmware and gets 20 KiB, so the same dump costs far fewer round
  trips. A stock device is left on the 4 KiB it always used.
- **An endpoint nobody answers is polled far less.** Stock declares an interrupt
  endpoint that the host asks a thousand times a second and that never replies.
  This asks for it every 16 ms instead.
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

Six things differ from the file it replaces:

- **It offers this firmware in the updater at all.** The Firmware Updater gains
  a choice between the original firmware and Open-GBFlash, each read from its
  own zip in `res/`, and asks you to confirm before installing this one. The
  project page appears there, in FlashGBX's **About Open-GBFlash** window and in
  the update-failure dialog.
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
  name stayed the same. Switching back to the stock firmware in the graphical
  updater does not normally need a U22 press. The command line updater is a
  different matter, covered under Installing: it cannot install this firmware,
  and it does need the button.
- **It knows what an update would install.** The firmware reports its own build
  date, so FlashGBX's table of stock build dates says nothing about it. This
  compares the device against the `fw_Open-GBFlash.zip` sitting beside it and
  offers an update only when that file is newer, rather than offering to replace
  this firmware with the stock one on every launch. To silence the prompt, put
  `SkipOpenFirmwareUpdate = enabled` in the `[General]` section of FlashGBX's
  `settings.ini`.

None of it changes how cartridges are read or written. The same file works with
the stock firmware, so there is no need to swap it back to use stock. The read
buffer is the one thing that changes with the firmware: on a stock device this
file keeps FlashGBX's own 4 KiB, on every platform.

## Installing

You need a GBFlash v1.0 to v1.3. Only v1.3 has been tested. Python is needed
only if you install FlashGBX with pip or run it from source; the Windows
downloads carry their own.

**Which FlashGBX you have matters**, because one of the files below replaces one
of its own.

- **Windows**: either download works. The portable `.zip` keeps its files in the
  folder you extracted it to, and the Setup package puts them in
  `C:\Users\<you>\AppData\Local\Programs\FlashGBX`.
- **macOS**: the app from the `.dmg` will **not** work. It is a frozen bundle
  with no Python files in it at all, so `hw_GBFlash.py` cannot be replaced and
  FlashGBX will not recognise the device afterwards. Install with pip instead,
  as below.
- **pip or a source checkout**: works everywhere.

If you are on something else, or unsure, the test is whether you can find
`hw_GBFlash.py` inside your FlashGBX. If it is not there, the build is frozen
and the firmware zip alone will not be enough.

### On macOS

The app from the `.dmg` cannot take `hw_GBFlash.py`, so install FlashGBX with
pip instead:

```
python3 -m pip install FlashGBX PySide6
```

`PySide6` is not optional. Without a Qt binding FlashGBX starts in command line
mode and there is no window to choose a firmware in. Naming it separately also
avoids `FlashGBX[qt6]`, which zsh treats as a filename pattern and refuses.

Start it with:

```
python3 -m FlashGBX
```

pip does install a `flashgbx` command, but in `~/Library/Python/<version>/bin`,
which macOS does not search by default, so `python3 -m FlashGBX` is the shorter
story. Both open the same window.

The two files go into the package directory. This prints where that is:

```
python3 -c "import FlashGBX, os; print(os.path.dirname(FlashGBX.__file__))"
```

### On Windows

Either download works, and both put the Python files one level down, in a
`FlashGBX` folder beside `FlashGBX.exe`:

- the portable `.zip`: wherever you extracted it
- the Setup package: `C:\Users\<you>\AppData\Local\Programs\FlashGBX`

Start it with `FlashGBX.exe` in that folder, or the Start menu entry the Setup
package creates.

### Installing the firmware

Keep a copy of the original `hw_GBFlash.py` before replacing it.

1. Download `fw_Open-GBFlash.zip` and `hw_GBFlash.py` from the releases page.
2. Put `fw_Open-GBFlash.zip` in FlashGBX's `res/` folder, **beside**
   `fw_GBFlash.zip`. Do not replace the original: it is what the **Original
   firmware** button installs. Overwrite it and that button installs this
   firmware instead; delete it and the Firmware Updater will not open at all.
   Then close FlashGBX, copy `hw_GBFlash.py` over the one sitting beside `res/`,
   and start it again. With FlashGBX still running the old file is already
   loaded and the firmware choice never appears.

   Where those two live:

   - **Windows**: `<install folder>\FlashGBX\`, so the zip goes in
     `<install folder>\FlashGBX\res\`.
   - **macOS, pip or source**: the package directory printed above.
   - The macOS `.app` has a `res/` folder under **right click the app > Show
     Package Contents > `Contents/MacOS/res/`**, but it cannot take
     `hw_GBFlash.py` at all, so the firmware zip alone will not help.
3. In FlashGBX, choose **Tools > Firmware Updater**, pick **Open-GBFlash** and
   confirm. That is the only way to install this firmware through FlashGBX. Its
   command line updater always writes `res/fw_GBFlash.zip`, the original
   firmware, whatever else is in `res/`: the file name is fixed in FlashGBX's own
   `FlashGBX_CLI.py`, which this project does not replace.

4. Only if the updater asks for it: unplug the device, hold the small **U22**
   button on the board, plug it back in while still holding. The blue **ACT**
   LED blinks twice, repeatedly.
5. Release the button and let the updater finish.

**If the device stops being detected later, FlashGBX was probably updated.**
`hw_GBFlash.py` belongs to the FlashGBX package, so `pip install --upgrade
FlashGBX` replaces it with the original, which does not know this firmware's USB
id. Copy it back. `fw_Open-GBFlash.zip` is not part of the package and survives.

**If the ACT LED does not blink twice repeatedly, the board probably has no
working bootloader.** Clone boards are often shipped without one, so that they
cannot be updated: an update would restore the vendor firmware's "cloned
hardware" warning, which the shipped image has had patched out. (Curiously, many
of them leave the registration speed limiting in place.)

That is only a problem for installing firmware, and it is fixable. There is an
open source bootloader that accepts updates:
<https://github.com/Daemon125/Open-GBFlash-Bootloader>. Install that first, then
come back to step 3.

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
make dist       # dist/fw_Open-GBFlash.zip, what FlashGBX installs
./tools/flash.sh
```

## Going back to the official firmware

The original firmware is still there. Installing this one never replaced it,
which is why `fw_Open-GBFlash.zip` goes beside `fw_GBFlash.zip` rather than over
it.

1. Open **Tools > Firmware Updater**.
2. Choose **Original firmware** and confirm.

No U22 press is needed in the graphical FlashGBX. The device is handed over on
the connection already open, and the updater then finds the bootloader by its
USB id rather than by the port name it had before. Confirmed on Windows and
macOS.

**The command line updater is different, and does need U22.** It finds the
device with its own port scan before anything else happens, and that scan looks
only for the original USB id, so it cannot see a device already running this
firmware. That code is FlashGBX's own and is not in the file this project
replaces.

The bootloader is never touched by any of this, so the U22 recovery path always
works even if a firmware image is bad. If the device stops enumerating entirely,
hold U22 while plugging in and reflash: that is the bootloader, not the
firmware, and it cannot be bricked by a bad update.

## Compatibility

Tested on GBFlash v1.3, on both an approved board and a clone board, with no
difference in speed or behaviour between them. Also run by a third party on an
approved board under Windows with FlashGBX 5.1.

Cartridges exercised:

- **Game Boy Advance**: ChisFlash 32 MiB flash cartridge with a 1M FLASH save,
  Pokemon Emerald repro, Super Mario Advance 4, Dragon Ball Z: The Legacy of
  Goku, and a Shark Tale GBA Video (3D Memory) cartridge. Super Mario Advance 4
  is a genuine mask ROM, read on all three methods and on both firmwares to the
  same md5, and checked against FlashGBX's own ROM database
- **Game Boy**: ChisFlash MBC3 2 MiB flash cartridge, a generic AliExpress
  flash cartridge, Pokemon Yellow repro, Pokemon Gold, Casper (MBC1) and
  Rugrats Time Travelers (MBC5). The last two are genuine mask ROMs, each read
  six times across both firmwares to the same md5
- **Game Boy, write-enable on the audio pin**: a DIY AM29F016 cartridge, ROM
  written and verified. Reported by a third party on an approved board, not
  tested here

Every read was compared byte for byte against a dump taken with the stock
firmware, and every write was verified.

### Not tested

The cartridge protocol and the bus waveforms are the stock ones, so these should
behave as they always did. They have simply never been in front of this
firmware:

- **Board revisions other than v1.3.** The installer accepts v1.0 to v1.3, but
  only v1.3 has been tested.
- **MBC2**, and the less common mappers: MBC6, MBC7, MMM01, HuC-1, HuC-3, TAMA5
  and the unlicensed ones. MBC1, MBC3 and MBC5 have been used.
- **Mapper-specific save paths** for chips like the one in Kirby Tilt 'n'
  Tumble, and the boot handshake some cartridges need. That code is transcribed
  from the stock firmware and has never run against the hardware it exists for.
- **Nintendo Power GB Memory cartridges**, Game Boy Camera, and cartridges with
  rumble or an accelerometer. Rumble is worth a word: there is no rumble-specific
  code in this firmware. FlashGBX stops the motor with ordinary cartridge writes,
  which are exercised constantly, so a rumble cart is untested rather than
  suspect.
- **Flash cartridges other than ChisFlash**, plus one generic AliExpress board.
  A different vendor's cart means a different flash chip, sector layout and
  unlock sequence, which is what varies between flash carts far more than the
  mapper does.
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

The firmware chooser in the updater is Charlie SIGMA's design, from a modified
`hw_GBFlash.py` he sent: the two options, the wording of the warning, the
confirmation defaulting to No, and disabling the controls while a write is
running. The packaging under it differs. His merged both firmwares into the
vendor's own zip; this one puts ours in its own zip beside it, so a FlashGBX
upgrade cannot take the choice away and a stock zip still opens the dialog.
