Carried patches
===============

`upstream/FlashGBX_LK_Firmware/LK.c` and `LK.h` are **byte-identical to
upstream and must stay that way**. `tools/lk_upstream_diff.py` hashes them and
fails `make lk-check` if either is edited in place. That is not bureaucracy: the
entire value of adopting Lesserkuma's skeleton is that upgrading stays a
three-file swap, and an inline edit is how that stops being true.

So everything this board needs that upstream does not have lives in exactly one
of three places, in this order of preference:

1. **`include/LK_device_ch579.h`**: anything expressible as a board macro.
   Most of the port is here.
2. **the framing front end** (`src/lk_glue.c`, `src/main.c`): anything above
   the command layer: framing, routing, interlocks, transport.
3. **a carried patch in this directory**: only what is a statement inside an
   `LK.c` function body, where neither of the above can reach.

There are three of category 3 and two of category 2 that are important enough
to be documented here rather than only in code.

| # | what | kind | applies to |
|---|------|------|-----------|
| [0001](0001-agb-save-flash-readback-poll.patch) | AGB save-FLASH: poll the byte back instead of `_delay_us(20)` | diff | `LK.c:1710` |
| [0002](0002-agb-address-latch-settle.patch) | AGB address-latch settle, five read paths (mismatch M1) | diff | `LK.c:1554,1573,1590,1741,2190` |
| [0003](0003-dmg-flash-we-pin-fallback.patch) | `FLASH_WE_PIN == 0` must still strobe `/WR` | diff | `LK.c:1257-1271` |
| [0004](0004-front-end-interlocks.md) | the 5 V interlock (M2) and the power-on settle (M3d) | front end | `src/lk_glue.c`, `src/main.c` |

Every one of them fixes something that **fails silently**: a corrupted save
that ACKs, a halfword-shifted dump that is self-consistent, a dropped write
strobe, a destroyed cartridge, a cartridge read before it has booted. That is
not a coincidence: a defect that fails loudly gets fixed upstream by whoever
hits it first, and only the quiet ones survive long enough to need carrying.

How they are applied
--------------------

The build copies `LK.c` to `build/lk/LK.c` and applies the three diffs there,
in order, with `patch --forward --batch --fuzz=0`. The vendored file is never
touched.
A hunk that does not apply is a **hard build failure**, not a warning.

The Makefile also defines `-DFW_LK_PATCH_0001` and friends, one per patch it
actually applied, and `include/LK_device_ch579.h` `#error`s without them. The
define list and the apply list come from the same `LK_PATCHES` variable, so
they cannot drift apart. Adding a fourth diff means adding it to that one list.

Regenerating them
-----------------

    python3 patches/make_patches.py            regenerate from the pinned LK.c
    python3 patches/make_patches.py --check    verify they are up to date

The diffs are generated from the pinned upstream text by exact,
uniqueness-checked string replacement, so an anchor that becomes ambiguous
after an upstream bump fails in the generator, where a human is looking,
rather than producing a patch that applies to the wrong line.

After an upstream bump
----------------------

1. `python3 patches/make_patches.py` and read the diff of the `.patch` files.
2. Re-read `LK.c`'s `LK_CMD_SET_VOLTAGE_5V` case and `lk_cart_power_on()` by
   hand. Patch 0004 has no anchors and therefore cannot fail loudly; this is
   the only thing standing behind it.
3. Re-run the hardware regression on real cartridges: a full AGB ROM read
byte-identical to a dump of the same cartridge taken before the bump, and the
same for DMG. `lk_upstream_diff.py` can
   prove a macro exists. It cannot prove the settle is still in front of the
   strobe.
