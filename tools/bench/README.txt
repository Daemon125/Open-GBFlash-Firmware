GBFlash firmware benchmark
==========================

Windows:        double-click RUN.bat
macOS / Linux:  ./run.sh

It flashes the stock GBFlash firmware and Open-GBFlash in turn, times the same
work through FlashGBX on each, and writes results/benchmark-<system>.txt.
Send that file back.

Roughly 15 minutes for reads only, 40 with writes.


What it asks for
----------------

Three cartridges, one at a time, each with a prompt and an Enter:

  1. a DMG (Game Boy) flash cartridge
  2. an AGB (Game Boy Advance) flash cartridge
  3. a 3D Memory cartridge (GBA Video, e.g. Shark Tale)

Type s and press Enter to skip a cartridge you do not have. Enter on its own
means the cartridge is seated, and starts that phase.


Writes erase the cartridge
--------------------------

Write benchmarks overwrite the DMG and AGB cartridges completely, saves
included. Back up anything on them first. The suite asks before doing any of
this. RUN-DMG-WRITE.bat answers for you, since a write run has nothing left to
do without it.

The 3D Memory cartridge is retail and read only. It is never written.


Why stock is measured twice
---------------------------

Stock is timed twice: once through FlashGBX's own hw_GBFlash.py, which pins the
read buffer to 0x1000, and again through the patched one this project ships,
which pins a stock device to 0x1000 as well. The two should agree, which is the
check that the host is not contributing to the firmware comparison.
Open-GBFlash has its own USB identity, which unmodified FlashGBX does not
recognise, so it is only measured on the patched host.


The test ROMs
-------------

mkroms.py builds them locally. They are pseudorandom with a valid header
checksum, no all-0xFF block, and no block shared between A and B, because
FlashGBX skips blocks that are blank or already match. Timing a write against
an ordinary ROM times the blanks it happens to contain rather than the
firmware. No game data is included or needed.


If something goes wrong
-----------------------

Python not found        install from python.org, tick "Add python.exe to PATH"
a module is missing     python -m pip install pyserial python-dateutil Pillow packaging requests
                        (RUN.bat does this for you; the suite checks before it starts)
device stops answering  hold the U22 button while plugging in, then run again
a write fails           pass the cart type, e.g.
                        RUN.bat --dmg-flashcart "ChisFlash MBC3 (2 MiB)"

Other switches: --quick (reads only, one method), --no-writes,
--skip dmg,agb,m3d.

The device is left running Open-GBFlash at the end. To put the stock firmware
back:  python flash_fw.py fw/stock_L15.bin
