#!/usr/bin/env python3
"""Regenerate the carried patches from the pinned upstream LK.c.

    python3 patches/make_patches.py            regenerate patches/*.patch
    python3 patches/make_patches.py --check     verify they are up to date

WHY THIS EXISTS RATHER THAN HAND-EDITED DIFFS.

A carried patch is only as good as its context lines: too few and it applies in
the wrong place, too many and it stops applying for an unrelated upstream edit.
Both failures are quiet if the patch is written by hand and never re-derived.

So the patches are GENERATED from the pinned upstream text by exact,
uniqueness-checked string replacement. If upstream moves in a way that makes an
anchor ambiguous or absent, this script fails here -- at the point where a human
is looking -- rather than producing a patch that applies to the wrong line.

The generated .patch files are what the build applies; this script is not run
by the build. Regenerating after an upstream bump is a deliberate act, and the
diff of the .patch files themselves is the review.
"""

import difflib
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
LK = os.path.join(FW, "upstream", "FlashGBX_LK_Firmware", "LK.c")


def sub1(text, old, new, what):
    n = text.count(old)
    if n != 1:
        sys.exit("patches/make_patches.py: anchor for %s matched %d times, "
                 "expected exactly 1.\n"
                 "Upstream has moved. Re-derive the anchor by hand, do not "
                 "loosen it.\n---\n%s\n---" % (what, n, old))
    return text.replace(old, new)


# --------------------------------------------------------------------------
# 0001 -- AGB save-FLASH read-back poll
# --------------------------------------------------------------------------
P1_HEADER = """\
carried patch 0001 -- AGB save-FLASH: poll the byte back, do not guess a delay

WHAT IT CHANGES
    lk_agb_cart_write_flash(), the LK_TYPE_FLASH_NON_ATMEL arm. Upstream waits
    a fixed _delay_us(20) after each programmed byte and then writes the next
    unlock sequence. This waits for the byte to read back instead.

WHY IT IS NOT OPTIONAL

    This is a bug this project has already shipped, reproduced and measured:

        "a short delay does not fail loudly: the next unlock sequence lands
         while the chip is still busy, gets swallowed, and the byte after it
         is written as data instead. 128 KiB of that erased the save to
         all-zeroes while every command still ACKed."
        -- src/cart.c:786-800, the comment on fw_cart_agb_sram_program()

    20 us is the TYPICAL byte-program time on these parts, not the maximum.
    The MX29L010 in the cartridge this firmware was developed against needs
    longer when the writes come back to back. The failure destroys a save file
    and reports success, which is the worst combination available.

    Upstream is not being careless -- it is faithfully reproducing the stock
    firmware. 27 nops in a loop IS _delay_us(1), so stock's `27 x 20` at flash
    0x6840 and LK.c:1710 are the same source compiled twice. The stock
    firmware has the same defect.

WHY IT CANNOT LIVE IN LK_device.h

    The delay is a statement inside a function body. No board macro is
    expanded anywhere near it: _delay_us() is, but redefining _delay_us()
    would change all 30-odd of its uses, including bus strobe widths where a
    poll would be catastrophic. There is no hook here and there is no macro
    whose meaning could be stretched to cover it without breaking something
    else.

    The poll itself IS in the header, as AGB_SAVE_FLASH_WAIT() -- only the
    call site has to be patched in.

UPSTREAM

    Worth proposing: read-back polling instead of a fixed delay, or a
    `_delay_agb_save_flash(addr, want)` hook. A merged upstream fix retires
    this patch and is worth much more than carrying it.

RISK IF IT SILENTLY STOPS APPLYING

    Silent save corruption on AGB FLASH saves. include/LK_device_ch579.h
    #errors without -DFW_LK_PATCH_0001, and the Makefile only defines that for
    patches it actually applied, so "stops applying" is a build failure.
"""

P1_OLD = (
"\t\t\tlk_agb_cart_write_flash_program_sequence();\n"
"\t\t\tlk_agb_cart_write_flash_byte(_lk_var32[LK_VAR32_ADDRESS]++, data_buffer[x]);\n"
"\t\t\t_delay_us(20);\n"
)
P1_NEW = (
"\t\t\tu16 _lk_wr_addr = (u16)_lk_var32[LK_VAR32_ADDRESS];\n"
"\t\t\tlk_agb_cart_write_flash_program_sequence();\n"
"\t\t\tlk_agb_cart_write_flash_byte(_lk_var32[LK_VAR32_ADDRESS]++, data_buffer[x]);\n"
"\t\t\tAGB_SAVE_FLASH_WAIT(_lk_wr_addr, data_buffer[x]); // carried patch 0001\n"
)


# --------------------------------------------------------------------------
# 0002 -- AGB address-latch settle
# --------------------------------------------------------------------------
P2_HEADER = """\
carried patch 0002 -- AGB address-latch settle (mismatch M1)

WHAT IT CHANGES
    Adds one _delay_agb_latch() call after the address latch in each of the
    five AGB read paths, before the first /RD strobe:

        lk_agb_cart_read_short              LK.c:1554   (Single)
        lk_agb_cart_read_data               LK.c:1573   (MemCpy)
        lk_agb_cart_read_data               LK.c:1590   (CPU / Stream)
        lk_agb_cart_read_data_3d_memory     LK.c:1741
        lk_dmg_agb_calc_crc32               LK.c:2190   (AGB arm)

WHY IT IS NOT OPTIONAL

    src/cart.c:238-268 is the record. With a cartridge in the slot, every AGB
    read came back shifted forward by exactly one halfword -- "POKEMON FIRE"
    read as "KEMON FIREBP", three passes agreeing perfectly. An empty slot
    cannot show it, because open bus is shifted the same as it is unshifted.

    The cause is the gap between the /CS store that latches the address and
    the first /RD. Stock spends 29 cycles there BY ACCIDENT: a pcb_ver check,
    a zero-length early-out and loop setup, none of it written down as a
    delay. LK.c does not have that accident -- all five paths go
    PIN_CS_L(); RAW_AGB_DATA_SET(0); RAW_AGB_DATA_DIR_IN(); PIN_RD_L(), three
    or four stores. So LK.c as shipped reproduces the shifted dump on this
    board.

    This is silent data corruption on the project's primary use case.

WHY IT IS A PATCH AND NOT A MACRO BODY

    It COULD hide inside RAW_AGB_DATA_DIR_IN(), which happens to be the last
    macro before the first /RD in all five paths. The draft of the board
    header did exactly that, and it is the wrong shape: the macro's name says
    "set the data direction" and says nothing about holding off a strobe. An
    upstream change that moves DIR_IN earlier, or strobes /RD without calling
    it, silently reintroduces a bug whose only symptom is a plausible,
    self-consistent, WRONG dump. tools/lk_upstream_diff.py cannot catch that;
    it proves a macro exists, not what it protects.

    As a patch, the five sites are named in a diff. If upstream moves any of
    them the patch fails to apply and the build stops -- a loud failure for a
    silent bug.

COST
    32 cycles per address latch. Per read method, because the three latch at
    different rates:
        CPU/Stream  once per CHUNK_MAX_LEN (64 B)  ->  0.5 cycles/byte, ~1%
        MemCpy      once per 4 bytes               ->  8 cycles/byte,  ~13%
        Single      once per 2 bytes               ->  16 cycles/byte, ~25%
    NOT a regression: the shipping firmware already pays exactly this
    (agb_latch_bytes() at main.c:308-315, fw_cart_agb_open()'s BUS_NOPS(32)).

UPSTREAM
    The honest version is a `_delay_agb_latch()` hook that LK.c calls itself,
    declared in the template as an empty macro. Worth proposing to Lesserkuma;
    it would help every board whose cartridges latch slower than the author's,
    and it would retire this patch.

RISK IF IT SILENTLY STOPS APPLYING
    Halfword-shifted AGB dumps that look fine. Guarded by FW_LK_PATCH_0002.
"""

P2_SITES = [
    ("Single, lk_agb_cart_read_short",
     "\tRAW_AGB_DATA_DIR_IN();\n\tPIN_RD_L();\n\t_delay_100ns();\n",
     "\tRAW_AGB_DATA_DIR_IN();\n\t_delay_agb_latch(); // carried patch 0002\n"
     "\tPIN_RD_L();\n\t_delay_100ns();\n"),
    ("MemCpy, lk_agb_cart_read_data",
     "\t\t\t\tRAW_AGB_DATA_DIR_IN();\n\t\t\t\tPIN_RD_L();\n\t\t\t\t_delay_300ns();\n",
     "\t\t\t\tRAW_AGB_DATA_DIR_IN();\n\t\t\t\t_delay_agb_latch(); // carried patch 0002\n"
     "\t\t\t\tPIN_RD_L();\n\t\t\t\t_delay_300ns();\n"),
    ("CPU/Stream, lk_agb_cart_read_data",
     "\t\t\tRAW_AGB_DATA_DIR_IN();\n\t\t\tfor (u32 x = 0; x < chunk_len >> 1; x++) {\n",
     "\t\t\tRAW_AGB_DATA_DIR_IN();\n\t\t\t_delay_agb_latch(); // carried patch 0002\n"
     "\t\t\tfor (u32 x = 0; x < chunk_len >> 1; x++) {\n"),
    ("3D Memory, lk_agb_cart_read_data_3d_memory",
     "\tRAW_AGB_DATA_DIR_IN();\n\n\tu8 client_ack = LK_CMD_AGB_CART_READ_3D_MEMORY;\n",
     "\tRAW_AGB_DATA_DIR_IN();\n\t_delay_agb_latch(); // carried patch 0002\n\n"
     "\tu8 client_ack = LK_CMD_AGB_CART_READ_3D_MEMORY;\n"),
    ("CRC32 AGB arm, lk_dmg_agb_calc_crc32",
     "\t\tRAW_AGB_DATA_DIR_IN();\n\t\tfor (u32 x = 0; x < length >> 1; x++) {\n",
     "\t\tRAW_AGB_DATA_DIR_IN();\n\t\t_delay_agb_latch(); // carried patch 0002\n"
     "\t\tfor (u32 x = 0; x < length >> 1; x++) {\n"),
]


# --------------------------------------------------------------------------
# 0003 -- DMG flash write: we == 0 must fall back to /WR
# --------------------------------------------------------------------------
P3_HEADER = """\
carried patch 0003 -- lk_dmg_flash_write_byte: an unset FLASH_WE_PIN must
                      still strobe something

WHAT IT CHANGES
    lk_dmg_flash_write_byte() (LK.c:1257-1271) selects the write strobe from
    _lk_var8[LK_VAR8_FLASH_WE_PIN] with three `if / else if` arms and NO else.
    A value outside {1,2,3} -- and 0 is the power-on value of that cell --
    drives the address and the data, waits 200 ns, and then issues no strobe
    at all. This adds the else: fall back to /WR.

WHY IT IS NOT OPTIONAL

    src/cart.c:552-556 already does this, deliberately: "treats an unset
    FLASH_WE_PIN as /WR". Taking LK.c unmodified retires that guard.

    The failure is the silent kind again. The address and data are driven, the
    command is acknowledged, and nothing reaches the cartridge -- so a flash
    write "succeeds" and the verify pass reports the cartridge as bad.

    RISK IS LOW AND THE COST IS ONE LINE. The host sets FLASH_WE_PIN in
    SET_FLASH_CMD before any flash write (LK.c:272, and FlashGBX always sends
    it), so the arm is not reached in normal operation. It is reached if a
    flash write is issued before SET_FLASH_CMD, or after a SET_VAR_STATE that
    restored a zero. Neither is exotic.

WHY IT CANNOT LIVE IN LK_device.h
    It is control flow inside a function body. There is no macro on the path.
    Defining FLASH_WE_PIN differently is not possible either -- it is a
    runtime variable the host writes, not a compile-time constant.

UPSTREAM
    Trivially proposable and hard to argue with: an else that matches the
    documented default.

RISK IF IT SILENTLY STOPS APPLYING
    Dropped DMG flash writes when the host has not set FLASH_WE_PIN.
    Guarded by FW_LK_PATCH_0003.
"""

P3_OLD = (
"\t} else if (_lk_var8[LK_VAR8_FLASH_WE_PIN] == LK_FLASH_WE_PIN_WR_RESET) {\n"
"\t\tPIN_CS2_L();\n"
"\t\tPIN_WR_L();\n"
"\t\t_delay_400ns();\n"
"\t\tPIN_WR_H();\n"
"\t\tPIN_CS2_H();\n"
"\t}\n"
"}\n"
)
P3_NEW = (
"\t} else if (_lk_var8[LK_VAR8_FLASH_WE_PIN] == LK_FLASH_WE_PIN_WR_RESET) {\n"
"\t\tPIN_CS2_L();\n"
"\t\tPIN_WR_L();\n"
"\t\t_delay_400ns();\n"
"\t\tPIN_WR_H();\n"
"\t\tPIN_CS2_H();\n"
"\t} else { // carried patch 0003: unset FLASH_WE_PIN falls back to /WR\n"
"\t\tPIN_WR_L();\n"
"\t\t_delay_400ns();\n"
"\t\tPIN_WR_H();\n"
"\t}\n"
"}\n"
)


def build(orig):
    p1 = sub1(orig, P1_OLD, P1_NEW, "0001 AGB save-flash poll")

    p2 = orig
    for what, old, new in P2_SITES:
        p2 = sub1(p2, old, new, "0002 latch settle: " + what)

    p3 = sub1(orig, P3_OLD, P3_NEW, "0003 we-pin fallback")
    return [("0001-agb-save-flash-readback-poll.patch", P1_HEADER, p1),
            ("0002-agb-address-latch-settle.patch", P2_HEADER, p2),
            ("0003-dmg-flash-we-pin-fallback.patch", P3_HEADER, p3)]


def diff(orig, new, header):
    body = "".join(difflib.unified_diff(
        orig.splitlines(keepends=True), new.splitlines(keepends=True),
        fromfile="a/LK.c", tofile="b/LK.c", n=6))
    return header + "\n" + body


def main():
    check = "--check" in sys.argv
    orig = io.open(LK, encoding="utf-8", newline="").read()
    bad = 0
    for name, header, new in build(orig):
        path = os.path.join(HERE, name)
        text = diff(orig, new, header)
        if check:
            have = io.open(path, encoding="utf-8", newline="").read() \
                if os.path.exists(path) else ""
            if have != text:
                print("STALE: %s" % name)
                bad += 1
        else:
            io.open(path, "w", encoding="utf-8", newline="").write(text)
            print("wrote %s (%d lines)" % (name, text.count("\n")))
    if check:
        print("patches are %s" % ("STALE" if bad else "up to date"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
