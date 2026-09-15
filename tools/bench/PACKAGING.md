# Building the distributable benchmark package

The scripts here are the whole benchmark. They need three things they do not
carry, because two are large and one is not ours to redistribute:

    flashgbx/                 a FlashGBX tree, with run.py at its root
    fw/stock_L15.bin          extracted from FlashGBX/res/fw_GBFlash.zip
    fw/gbflash_open.bin       build/fw.bin from `make`
    gbflash_serial_update.py  from the GBFlash Unlocker project, which is not
                              part of this repository

To build the zip that goes to a test machine:

    PKG=/tmp/gbflash-bench
    mkdir -p $PKG/fw $PKG/hosts $PKG/results
    cp tools/bench/*.py tools/bench/RUN.bat tools/bench/run.sh \
       tools/bench/README.txt $PKG/
    cp tools/bench/hosts/*.py $PKG/hosts/
    cp hw_GBFlash.py $PKG/hosts/hw_GBFlash_patched.py
    cp -R <a FlashGBX tree> $PKG/flashgbx      # one with run.py at its root
    (cd $PKG/flashgbx && \
     patch -p1 --forward --fuzz=0 < <repo>/tools/flashgbx/gbflash_open_readmethod.patch && \
     patch -p1 --forward --fuzz=0 < <repo>/tools/flashgbx/gbflash_open_writespeed.patch)
    cp ../fw/fw.bin $PKG/fw/stock_L15.bin
    cp build/fw.bin $PKG/fw/gbflash_open.bin
    cp ../ref/gbflash_unlocker/gbflash_serial_update.py $PKG/
    find $PKG -name '__pycache__' -type d -exec rm -rf {} +
    find $PKG -name 'bootlogo_*.bin' -delete
    cp $PKG/hosts/hw_GBFlash_patched.py $PKG/flashgbx/FlashGBX/hw_GBFlash.py
    (cd $(dirname $PKG) && zip -qr gbflash-bench.zip $(basename $PKG))

Those two patches are the ones that touch files other than hw_GBFlash.py, so
copying the host file over cannot carry them. Without readmethod the CLI keeps
its hardcoded SetAGBReadMethod(method=2), every AGB read row measures Stream,
and the Single row is a duplicate under a wrong label: the numbers look
plausible and the dumps still match, so nothing announces it. bench_suite.py
refuses to start without it. Without writespeed the write rows measure the
unpatched write path. The other four patches only touch hw_GBFlash.py, which
the copy below replaces wholesale.

hosts/hw_GBFlash_patched.py is a copy of the root hw_GBFlash.py, which is
the one that ships to users. Keeping a second copy in tools/bench would let
the two drift, and the harness would then measure a host file nobody runs.

bootlogo_*.bin must not ship. FlashGBX writes one there after reading any real
cartridge, and its presence turns a warning about the test ROMs' absent logo
into an interactive prompt. bench_suite.py deletes it at startup and before
every write, so a stale one is not fatal, but there is no reason to carry it.

## Flashcart profiles

The DMG write fallbacks in bench_suite.py name ChisFlash profiles. FlashGBX does
not ship all of them, and one of them was written for a specific board and is
not in this repository. A name FlashGBX does not know shows up as a failed write
listing the profiles it does know, so pass --dmg-flashcart with a name from that
list rather than expecting the fallbacks to cover every board.
