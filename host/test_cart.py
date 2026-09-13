#!/usr/bin/env python3
"""src/cart.c, compiled on the host with REG32() redirected into RAM
(mmio_shim.c) and asserted on the bits that would reach the pins.

No cartridge and no bus here: these tests say which pins get driven and when,
never that a dump is correct.
"""

import ctypes
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

R32_PA_DIR = 0x400010A0
R32_PA_PIN = 0x400010A4
R32_PA_OUT = 0x400010A8
R32_PB_DIR = 0x400010C0
R32_PB_PIN = 0x400010C4
R32_PB_OUT = 0x400010C8
R32_PB_CLR = 0x400010CC

PA_AD_MASK = 0xFFFF
PB_DATA = 0xFF
PB_LED = 1 << 12
PB_WR = 1 << 13
PB_RD = 1 << 14
PB_CS = 1 << 15
PB_CLK = 1 << 18
PB_CS2 = 1 << 19
PB_AUDIO = 1 << 20
PB_VSEL = 1 << 21
PB_VCC_EN = 1 << 22

DMG_RD, DMG_A15, DMG_SLOW_A15 = 0, 1, 2

_checks = 0
_fails = 0


def section(name):
    print(name)


def ck(cond, what, detail=""):
    global _checks, _fails
    _checks += 1
    if not cond:
        _fails += 1
        print("  FAIL %s%s" % (what, ("  [%s]" % detail) if detail else ""))


def build(tmp):
    so = os.path.join(tmp, "libcart.so")
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-std=c99", "-g", "-O1", "-fPIC", "-shared",
           "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-DREG32(a)=(*fw_test_reg((uintptr_t)(a)))",
           # cart.h declares these routines unconditionally, so the
           # exhaustiveness check below demands them in the object. cart.c
           # rejects FW_AGB_LEAF=1 without FW_AGB_FAST_BURST.
           "-DFW_AGB_LEAF=1", "-DFW_AGB_FAST_BURST=1",
           "-DFW_DMG_WRITE_BURST=1", "-DFW_DMG_POLL_TIGHT=1",
           # The Makefile's shipped defaults, so cart.c here is the cart.c in
           # the image.
           "-DFW_CART_PULLUPS_FULL=1", "-DFW_CART_AUDIO_WE_SAFE=1",
           "-DFW_CART_AUDIO_HONOUR=1", "-DFW_DMG_A15_PAD=1",
           "-DFW_DMG_WRITE_RAW_PAD=1", "-DFW_DMG_CS_READ_PAD=1",
           "-include", os.path.join(ROOT, "host", "mmio_shim.h"),
           "-I", os.path.join(ROOT, "include"),
           "-I", os.path.join(ROOT, "host"),
           "-o", so,
           os.path.join(ROOT, "src", "cart.c"),
           os.path.join(ROOT, "host", "mmio_shim.c")]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode != 0:
        sys.stdout.write(p.stdout.decode("utf-8", "replace"))
        sys.exit("could not build src/cart.c for the host")
    return so


class Cart(object):
    def __init__(self, so):
        self.lib = ctypes.CDLL(so)
        self.lib.fw_test_get.restype = ctypes.c_uint32
        self.lib.fw_test_get.argtypes = [ctypes.c_void_p]
        self.lib.fw_test_set.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self.lib.fw_test_oob.restype = ctypes.c_uint32
        self.lib.fw_cart_voltage.restype = ctypes.c_int

    def reset(self):
        self.lib.fw_test_reset()

    def get(self, addr):
        return self.lib.fw_test_get(ctypes.c_void_p(addr))

    def set(self, addr, val):
        self.lib.fw_test_set(ctypes.c_void_p(addr), ctypes.c_uint32(val))

    def buf(self, n):
        return (ctypes.c_uint8 * n)()


def test_five_volts_is_refused_outside_dmg_mode(c):
    section("fw_cart_voltage: the guard, tested where the guard actually is")
    for five, dmg, want_vsel, label in (
            (1, 0, False, "5 V asked for in AGB mode"),
            (1, 0, False, "5 V asked for with no mode chosen"),
            (0, 1, False, "3.3 V asked for in DMG mode"),
            (0, 0, False, "3.3 V asked for in AGB mode"),
            (1, 1, True,  "5 V asked for in DMG mode")):
        c.reset()
        c.lib.fw_cart_voltage(five, dmg)
        hot = bool(c.get(R32_PB_OUT) & PB_VSEL)
        ck(hot == want_vsel,
           "%s -> PB21 %s" % (label, "high" if want_vsel else "stays low"),
           "PB_OUT=0x%08X" % c.get(R32_PB_OUT))
    c.reset()
    ck(c.lib.fw_cart_voltage(1, 0) == 0,
       "and it reports the refusal rather than claiming 5 V")
    c.reset()
    ck(c.lib.fw_cart_voltage(1, 1) == 1, "while a granted 5 V reports 5 V")


def test_nothing_else_in_cart_c_can_raise_the_rail(c):
    section("no other entry point drives PB21 high")
    # The shim never lowers a PB_OUT bit, so a `|= PB_VSEL` undone a line
    # later still fails here. Gap: agb_burst_fast() and
    # fw_cart_agb_read_leaf() cache PB_OUT into `hi` and re-store the whole
    # register per strobe, erasing a bit raised after that capture.
    out = c.buf(64)
    calls = [
        ("fw_cart_init", lambda: c.lib.fw_cart_init()),
        ("fw_cart_power(1)", lambda: c.lib.fw_cart_power(1)),
        ("fw_cart_power(0)", lambda: c.lib.fw_cart_power(0)),
        ("fw_cart_settle", lambda: c.lib.fw_cart_settle()),
        ("fw_cart_tristate", lambda: c.lib.fw_cart_tristate()),
        ("fw_cart_pullups(1)", lambda: c.lib.fw_cart_pullups(1)),
        ("fw_cart_pullups(0)", lambda: c.lib.fw_cart_pullups(0)),
        ("fw_cart_agb_open", lambda: c.lib.fw_cart_agb_open(0x1234)),
        ("fw_cart_agb_burst", lambda: c.lib.fw_cart_agb_burst(out, 16)),
        ("fw_cart_agb_close", lambda: c.lib.fw_cart_agb_close()),
        ("fw_cart_agb_read_leaf",
         lambda: c.lib.fw_cart_agb_read_leaf(0x1234, out, 8, 1)),
        ("fw_cart_agb_write", lambda: c.lib.fw_cart_agb_write(0x1234, 0xAA55)),
        ("fw_cart_agb_write_burst",
         lambda: c.lib.fw_cart_agb_write_burst(0x1234, out, 8)),
        ("fw_cart_dmg_setup", lambda: c.lib.fw_cart_dmg_setup()),
        ("fw_cart_dmg_mbc_reset", lambda: c.lib.fw_cart_dmg_mbc_reset()),
        ("fw_cart_clk_pulses", lambda: c.lib.fw_cart_clk_pulses(4)),
        ("fw_cart_dmg_write", lambda: c.lib.fw_cart_dmg_write(0x2100, 1, 0)),
        ("fw_cart_dmg_write cs", lambda: c.lib.fw_cart_dmg_write(0xA000, 1, 1)),
        ("fw_cart_dmg_flash_write",
         lambda: c.lib.fw_cart_dmg_flash_write(0x5555, 0xAA, 0)),
        ("fw_cart_dmg_amd_program_byte",
         lambda: c.lib.fw_cart_dmg_amd_program_byte(
             (ctypes.c_uint32 * 3)(0x5555, 0x2AAA, 0x5555),
             (ctypes.c_uint16 * 3)(0xAA, 0x55, 0xA0), 0x1234, 0x5A)),
        ("fw_cart_dmg_amd_program_buffer",
         lambda: c.lib.fw_cart_dmg_amd_program_buffer(
             (ctypes.c_uint32 * 6)(0x5555, 0x2AAA, 0, 0, 0, 0),
             (ctypes.c_uint16 * 6)(0xAA, 0x55, 0x25, 0, 0, 0x29),
             0x1234, 4, (ctypes.c_uint8 * 4)(1, 2, 3, 4))),
        ("fw_cart_dmg_amd_bypass_enter",
         lambda: c.lib.fw_cart_dmg_amd_bypass_enter(
             (ctypes.c_uint32 * 2)(0x5555, 0x2AAA),
             (ctypes.c_uint16 * 2)(0xAA, 0x55))),
        ("fw_cart_dmg_amd_bypass_byte",
         lambda: c.lib.fw_cart_dmg_amd_bypass_byte(0xA0, 0x1234, 0x5A)),
        ("fw_cart_dmg_amd_bypass_exit",
         lambda: c.lib.fw_cart_dmg_amd_bypass_exit(0x1234)),
        ("fw_cart_dmg_write_burst_release",
         lambda: c.lib.fw_cart_dmg_write_burst_release()),
        ("fw_cart_dmg_status_poll_open",
         lambda: c.lib.fw_cart_dmg_status_poll_open()),
        ("fw_cart_dmg_status_poll_read",
         lambda: c.lib.fw_cart_dmg_status_poll_read(0xA000)),
        ("fw_cart_dmg_status_poll_close",
         lambda: c.lib.fw_cart_dmg_status_poll_close()),
        ("fw_cart_dmg_pulse_reset", lambda: c.lib.fw_cart_dmg_pulse_reset()),
        ("fw_cart_agb_sram_open", lambda: c.lib.fw_cart_agb_sram_open()),
        ("fw_cart_agb_sram_close", lambda: c.lib.fw_cart_agb_sram_close()),
        ("fw_cart_agb_sram_read", lambda: c.lib.fw_cart_agb_sram_read(0, out, 16)),
        ("fw_cart_agb_sram_write", lambda: c.lib.fw_cart_agb_sram_write(0, 0xA5)),
        ("fw_cart_agb_peek", lambda: c.lib.fw_cart_agb_peek(0x1234)),
        ("fw_cart_delay_nops", lambda: c.lib.fw_cart_delay_nops(8)),
        ("fw_cart_set_we_pin", lambda: c.lib.fw_cart_set_we_pin(1)),
        ("fw_cart_agb_sram_program",
         lambda: c.lib.fw_cart_agb_sram_program(0, out, 16, 1)),
        ("fw_cart_agb_eeprom_bus", lambda: c.lib.fw_cart_agb_eeprom_bus()),
        ("fw_cart_agb_eeprom_read",
         lambda: c.lib.fw_cart_agb_eeprom_read(0, out, 2)),
        ("fw_cart_agb_eeprom_write",
         lambda: c.lib.fw_cart_agb_eeprom_write(0, out, 2)),
        ("fw_cart_agb_rtc_read", lambda: c.lib.fw_cart_agb_rtc_read(out)),
        ("fw_cart_agb_bootup", lambda: c.lib.fw_cart_agb_bootup()),
        ("fw_cart_agb_3d_open", lambda: c.lib.fw_cart_agb_3d_open(0x1234, 0x1000)),
        ("fw_cart_agb_3d_read", lambda: c.lib.fw_cart_agb_3d_read(out, 16)),
        ("fw_cart_agb_3d_close", lambda: c.lib.fw_cart_agb_3d_close()),
        ("fw_cart_audio_drive", lambda: c.lib.fw_cart_audio_drive(1)),
        ("fw_cart_audio_dir", lambda: c.lib.fw_cart_audio_dir(0)),
    ]

    for method in (DMG_RD, DMG_A15, DMG_SLOW_A15):
        for pulse in (0, 1):
            calls.append(("fw_cart_dmg_read m=%d cs=%d" % (method, pulse),
                          (lambda m=method, p=pulse:
                           c.lib.fw_cart_dmg_read(0x0100, out, 16, m, p))))
    hdr = open(os.path.join(os.path.dirname(__file__), "..", "include",
                            "cart.h")).read()
    exported = set(re.findall(r"^[A-Za-z_]\w*\s+(fw_cart_[a-z0-9_]+)\s*\(",
                              hdr, re.M))
    # Driving PB21 is fw_cart_voltage()'s job; it is covered above.
    exported.discard("fw_cart_voltage")
    covered = {n.split("(")[0].split(" ")[0] for n, _ in calls}
    missing = sorted(exported - covered)
    ck(not missing, "every exported bus routine is in the list above",
       "not called: " + ", ".join(missing))

    for name, fn in calls:
        c.reset()
        fn()
        ck(not (c.get(R32_PB_OUT) & PB_VSEL), "%s leaves PB21 low" % name,
           "PB_OUT=0x%08X" % c.get(R32_PB_OUT))
        ck(c.lib.fw_test_oob() == 0,
           "%s touches no register outside the GPIO block" % name)


def test_no_read_path_ever_asserts_a_write_strobe(c):
    section("reading never pulses /WR or AUDIO")
    # A read that asserts /WR writes whatever the data lines hold into
    # whatever the address lines select, silently corrupting a battery-backed
    # save. AUDIO is the write-enable when FLASH_WE_PIN is 0x02
    # (LK_Device.py:4304-4307). The shim never lowers a PB_CLR bit, so a
    # strobe raised and released inside the routine still fails here.
    out = c.buf(4096)
    readers = [
        ("fw_cart_agb_open+burst+close",
         lambda: (c.lib.fw_cart_agb_open(0x1234),
                  c.lib.fw_cart_agb_burst(out, 64),
                  c.lib.fw_cart_agb_close())),
        # Group sizes: Single 1 halfword, MemCpy 2, Stream 64.
        ("fw_cart_agb_read_leaf grp=1",
         lambda: c.lib.fw_cart_agb_read_leaf(0x1234, out, 32, 1)),
        ("fw_cart_agb_read_leaf grp=2",
         lambda: c.lib.fw_cart_agb_read_leaf(0x1234, out, 32, 2)),
        ("fw_cart_agb_read_leaf grp=64",
         lambda: c.lib.fw_cart_agb_read_leaf(0x1234, out, 32, 64)),
        ("fw_cart_agb_peek", lambda: c.lib.fw_cart_agb_peek(0x1234)),
        ("fw_cart_agb_sram_read",
         lambda: (c.lib.fw_cart_agb_sram_open(),
                  c.lib.fw_cart_agb_sram_read(0, out, 64),
                  c.lib.fw_cart_agb_sram_close())),
        # 3d_open is excluded: programming the mapper registers is a write and
        # must strobe /WR. The burst that follows it must not.
        ("fw_cart_agb_3d_read", lambda: c.lib.fw_cart_agb_3d_read(out, 64)),
    ]
    for method in (DMG_RD, DMG_A15, DMG_SLOW_A15):
        for pulse in (0, 1):
            readers.append(("fw_cart_dmg_read m=%d cs=%d" % (method, pulse),
                            (lambda m=method, p=pulse:
                             c.lib.fw_cart_dmg_read(0x0100, out, 64, m, p))))
    for name, fn in readers:
        for we in (0, 1, 2, 3):
            # The selector is latched for the session, so a read that consults
            # it would misbehave on some cartridges only.
            c.reset()
            c.lib.fw_cart_set_we_pin(we)
            c.reset()
            fn()
            clr = c.get(R32_PB_CLR)
            ck(not (clr & PB_WR), "%s (we=%d) never asserts /WR" % (name, we),
               "PB_CLR=0x%08X" % clr)
            ck(not (clr & PB_AUDIO),
               "%s (we=%d) never asserts AUDIO" % (name, we),
               "PB_CLR=0x%08X" % clr)


def test_agb_save_access_turns_the_bus_around_safely(c):
    section("AGB save memory: the bus turnaround, in the order that matters")
    # The save chip's data lines are the ROM bus's A16..A23 and both ends can
    # drive them. Release the local drivers before arming the address, and
    # park the address before re-driving the data lines.
    c.reset()
    c.lib.fw_cart_agb_sram_open()
    ck(not (c.get(R32_PB_DIR) & PB_DATA),
       "open releases D0..D7 so the save chip can drive them",
       "PB_DIR=0x%08X" % c.get(R32_PB_DIR))
    ck(c.get(R32_PB_CLR) & PB_DATA,
       "having driven them low first, not left them at whatever they held")
    ck((c.get(R32_PA_DIR) & PA_AD_MASK) == PA_AD_MASK,
       "and arms A0..A15, which nothing else on this path does")

    c.reset()
    c.lib.fw_cart_agb_sram_close()
    ck(c.get(R32_PA_OUT) == 0,
       "close parks the address before the data lines turn back around",
       "PA_OUT=0x%08X" % c.get(R32_PA_OUT))
    ck(c.get(R32_PB_DIR) & PB_DATA, "and only then re-drives D0..D7")

    # /CS2 is PB19, the same pin DMG calls /RESET. It must be released.
    out = c.buf(16)
    c.reset()
    c.lib.fw_cart_agb_sram_read(0x0000, out, 16)
    ck(c.get(R32_PB_CLR) & PB_CS2, "a save read selects the chip via /CS2")
    ck(c.get(R32_PB_OUT) & PB_CS2, "and deselects it afterwards")
    ck(c.get(R32_PB_OUT) & PB_RD,
       "and releases /RD, so the save chip stops driving the bus")
    c.reset()
    c.lib.fw_cart_agb_sram_write(0x0000, 0xA5)
    ck(c.get(R32_PB_OUT) & PB_RD,
       "a save write raises /RD before driving data. The read path leaves "
       "it low across a whole block on purpose")
    ck(c.get(R32_PB_DIR) & PB_DATA, "then drives D0..D7")
    ck(c.get(R32_PB_CLR) & PB_WR, "and strobes /WR")
    ck(c.get(R32_PB_OUT) & PB_CS2, "leaving the chip deselected")


def test_the_write_enable_pin_is_selectable(c):
    section("FLASH_WE_PIN moves the DMG FLASH write strobe, and only that one")
    # Write-enable is AUDIO (PB20) on some cartridges and /WR (PB13) on
    # others, selected by FLASH_WE_PIN in DMG mode only (LK_Device.py:770-775).
    # The selector applies to flash writes (LK.c:1246-1272), never to an MBC
    # bank select (LK.c:1087-1125); 0x2100 is an MBC5 ROM-bank register.
    c.reset()
    c.lib.fw_cart_set_we_pin(1)
    c.lib.fw_cart_dmg_flash_write(0x5555, 0xAA, 0)
    ck(c.get(R32_PB_CLR) & PB_WR, "we=1 strobes /WR")
    ck(not (c.get(R32_PB_CLR) & PB_AUDIO), "and leaves AUDIO alone")

    c.reset()
    c.lib.fw_cart_set_we_pin(2)
    c.lib.fw_cart_dmg_flash_write(0x5555, 0xAA, 0)
    ck(c.get(R32_PB_CLR) & PB_AUDIO, "we=2 strobes AUDIO instead",
       "PB_CLR=0x%08X" % c.get(R32_PB_CLR))
    ck(not (c.get(R32_PB_CLR) & PB_WR), "and leaves /WR alone")
    ck(c.get(R32_PB_OUT) & PB_AUDIO, "releasing it when the pulse ends")

    # Merge the two paths and with we=2 latched every bank select strobes
    # AUDIO: the bank never changes and the whole ROM lands in bank 1's window.
    c.reset()
    c.lib.fw_cart_dmg_write(0x2100, 0x02, 0)
    ck(c.get(R32_PB_CLR) & PB_WR,
       "an ordinary cartridge write still strobes /WR with we=2 latched",
       "PB_CLR=0x%08X" % c.get(R32_PB_CLR))
    ck(not (c.get(R32_PB_CLR) & PB_AUDIO), "and does not touch AUDIO")

    # we=3 is "WR+RESET": /CS2 (PB19) is held around the /WR pulse.
    # re/symbols-cartio.md:520-528 (stock 0x87DA), LK.c:1265-1271.
    c.reset()
    c.lib.fw_cart_set_we_pin(3)
    c.lib.fw_cart_dmg_flash_write(0x5555, 0xAA, 0)
    ck(c.get(R32_PB_CLR) & PB_CS2, "we=3 drops /CS2 around the pulse")
    ck(c.get(R32_PB_CLR) & PB_WR, "and still strobes /WR")
    ck(c.get(R32_PB_OUT) & PB_CS2, "releasing /CS2 afterwards")

    c.reset()
    c.lib.fw_cart_set_we_pin(0)
    c.lib.fw_cart_dmg_flash_write(0x5555, 0xAA, 0)
    ck(c.get(R32_PB_CLR) & PB_WR, "an unknown selector falls back to /WR")
    c.lib.fw_cart_set_we_pin(1)


def test_audio_pin_is_a_direction_control(c):
    section("the AUDIO/IRQ line is released by default and only driven on request")
    # PB_AUDIO is pin 31 on AGB (IRQ) and audio-in on DMG; a cartridge may
    # drive it, so arming it push-pull in fw_cart_init() contends for the whole
    # session. lk_dmg_flash_enable_audio() (LK.c:235-248) changes level and
    # direction, the FLASH_WE_PIN path (LK.c:228-233) direction only. Merged,
    # fw_cart_set_we_pin() writes PB_CLR and moves a line nobody asked to move.
    c.reset()
    c.lib.fw_cart_init()
    ck(not (c.get(R32_PB_DIR) & PB_AUDIO),
       "fw_cart_init leaves AUDIO an input, not a driven output")

    c.reset()
    c.lib.fw_cart_audio_drive(1)
    ck(c.get(R32_PB_DIR) & PB_AUDIO, "audio_drive(1) makes it an output")
    ck(c.get(R32_PB_OUT) & PB_AUDIO, "and drives it high")

    c.reset()
    c.lib.fw_cart_audio_drive(0)
    ck(c.get(R32_PB_CLR) & PB_AUDIO, "audio_drive(0) drives it low first")
    ck(not (c.get(R32_PB_DIR) & PB_AUDIO), "then releases it to an input")

    c.reset()
    c.lib.fw_cart_audio_dir(1)
    ck(c.get(R32_PB_DIR) & PB_AUDIO, "audio_dir(1) makes it an output")
    ck(not (c.get(R32_PB_CLR) & PB_AUDIO) and not (c.get(R32_PB_OUT) & PB_AUDIO),
       "and touches neither PB_OUT nor PB_CLR")

    # Selecting AUDIO must arm the pin, or the 26 shipped profiles with
    # "write_pin":"AUDIO" cannot strobe at all.
    c.reset()
    c.lib.fw_cart_set_we_pin(2)          # FW_DMG_WE_AUDIO
    ck(c.get(R32_PB_DIR) & PB_AUDIO,
       "selecting AUDIO as the write-enable arms the pin as an output")
    c.reset()
    c.lib.fw_cart_set_we_pin(1)          # /WR
    ck(not (c.get(R32_PB_DIR) & PB_AUDIO),
       "and selecting /WR releases it again")


def test_power_off_drops_the_five_volt_select(c):
    section("powering the slot down takes the 5 V select down with it")
    # Cartridges are swapped at power-off; a latched select leaves a 5 V rail
    # waiting for whatever goes in next.
    c.reset()
    c.lib.fw_cart_voltage(1, 1)
    ck(c.get(R32_PB_OUT) & PB_VSEL, "5 V selected in DMG mode")
    c.lib.fw_cart_power(0)
    ck(c.get(R32_PB_CLR) & PB_VSEL,
       "and power-off clears PB21", "PB_CLR=0x%08X" % c.get(R32_PB_CLR))
    ck(c.get(R32_PB_CLR) & PB_VCC_EN, "along with the rail enable")


def test_the_dmg_read_prologue_asserts_rd(c):
    section("every DMG read variant output-enables the cartridge ROM")
    # The cartridge mask ROM's /OE is /RD on pin 4. Without it the A15 and
    # SlowA15 variants sample a floating bus.
    out = c.buf(16)
    for method in (DMG_RD, DMG_A15, DMG_SLOW_A15):
        c.reset()
        c.lib.fw_cart_dmg_read(0x0100, out, 16, method, 0)
        ck(c.get(R32_PB_CLR) & PB_RD,
           "method %d drives /RD low" % method,
           "PB_CLR=0x%08X" % c.get(R32_PB_CLR))
        ck(c.get(R32_PB_OUT) & PB_RD,
           "method %d releases /RD on the way out" % method,
           "PB_OUT=0x%08X" % c.get(R32_PB_OUT))
    # Stock reaches its shared tail even when the count is zero (0x7EF8 beq
    # 0x7F64), so an empty read must not leave the ROM enabled either.
    c.reset()
    c.lib.fw_cart_dmg_read(0x0100, out, 0, DMG_RD, 0)
    ck(c.get(R32_PB_OUT) & PB_RD, "a zero-length read still releases /RD")


def test_the_dmg_write_arms_the_address_bus(c):
    section("fw_cart_dmg_write drives PA_DIR itself, as stock does")
    # Three routines clear PA_DIR and none restore it; stock re-arms inside the
    # write (0x80E6-0x80F0). Without that the first write after any tristate
    # goes out on a Hi-Z bus, lands nowhere, and still ACKs: a dropped bank
    # select silently re-dumps the previous bank.
    for label, pre in (("after tristate", lambda: c.lib.fw_cart_tristate()),
                       ("after power-off", lambda: c.lib.fw_cart_power(0)),
                       ("from cold init", lambda: c.lib.fw_cart_init())):
        c.reset()
        pre()
        ck(not (c.get(R32_PA_DIR) & PA_AD_MASK),
           "%s the address bus really is tri-stated" % label)
        c.lib.fw_cart_dmg_write(0x2100, 0x01, 0)
        ck((c.get(R32_PA_DIR) & PA_AD_MASK) == PA_AD_MASK,
           "%s the write arms A0..A15 before driving them" % label,
           "PA_DIR=0x%08X" % c.get(R32_PA_DIR))
        ck(c.get(R32_PA_OUT) == 0x2100,
           "%s and the address reaches the pins" % label,
           "PA_OUT=0x%08X" % c.get(R32_PA_OUT))


def test_the_dmg_write_raises_rd_before_driving_data(c):
    section("fw_cart_dmg_write releases /RD before it drives D0..D7")
    # SUBSYSTEM-SPECS hazard H1: a read leaves /RD asserted on purpose, so an
    # ordering mistake drives the data bus into a still-enabled cartridge ROM.
    # Two CMOS output stages fight at tens of mA per pin.
    c.reset()
    out = c.buf(4)
    c.lib.fw_cart_dmg_read(0x0100, out, 4, DMG_A15, 0)
    c.set(R32_PB_OUT, c.get(R32_PB_OUT) & ~PB_RD)   # pretend /RD is still low
    c.lib.fw_cart_dmg_write(0x2100, 0x01, 0)
    ck(c.get(R32_PB_OUT) & PB_RD,
       "the write asserts /RD high rather than assuming it",
       "PB_OUT=0x%08X" % c.get(R32_PB_OUT))
    ck(not (c.get(R32_PB_DIR) & PB_DATA),
       "and hands D0..D7 back as inputs when it is done. A data bus left "
       "driven makes the next read return what we wrote",
       "PB_DIR=0x%08X" % c.get(R32_PB_DIR))


def test_power_up_settles_before_anything_is_driven(c):
    section("the rail comes up before any line is driven into the cartridge")
    # A control line driven high into an unpowered cartridge sources current
    # through its input protection and can back-power the part.
    c.reset()
    c.lib.fw_cart_power(1)
    ck(c.get(R32_PB_OUT) & PB_VCC_EN, "VCC is enabled")
    ck(c.get(R32_PB_OUT) & (PB_RD | PB_WR | PB_CS),
       "the control lines are deasserted, not left low",
       "PB_OUT=0x%08X" % c.get(R32_PB_OUT))
    ck(not (c.get(R32_PA_DIR) & PA_AD_MASK),
       "and the address bus is NOT re-armed here. That would drive a stale "
       "16-bit address into the cartridge the instant VCC came up")


def test_init_leaves_the_slot_cold_and_at_3v3(c):
    section("fw_cart_init: nothing energised, 3.3 V selected")
    c.reset()
    c.lib.fw_cart_init()
    ck(not (c.get(R32_PB_OUT) & PB_VCC_EN), "the rail is not enabled at boot")
    ck(not (c.get(R32_PB_OUT) & PB_VSEL), "and 3.3 V is the resting selection")
    ck(c.get(R32_PB_CLR) & PB_VSEL, "PB21 is positively driven low, not left")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        c = Cart(build(tmp))
        for t in (test_five_volts_is_refused_outside_dmg_mode,
                  test_nothing_else_in_cart_c_can_raise_the_rail,
                  test_no_read_path_ever_asserts_a_write_strobe,
                  test_agb_save_access_turns_the_bus_around_safely,
                  test_the_write_enable_pin_is_selectable,
                  test_audio_pin_is_a_direction_control,
    test_power_off_drops_the_five_volt_select,
                  test_the_dmg_read_prologue_asserts_rd,
                  test_the_dmg_write_arms_the_address_bus,
                  test_the_dmg_write_raises_rd_before_driving_data,
                  test_power_up_settles_before_anything_is_driven,
                  test_init_leaves_the_slot_cold_and_at_3v3):
            t(c)
    print("test_cart: %d checks, %d failures" % (_checks, _fails))
    return 1 if _fails else 0


if __name__ == "__main__":
    sys.exit(main())
