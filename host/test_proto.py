#!/usr/bin/env python3
"""Drive src/proto.c natively and check what FlashGBX would make of it.

    python3 host/test_proto.py          (or: make -C host test)

src/proto.c is compiled for the host as a shared library and called through
ctypes. Requires a C compiler and nothing else.

The wire protocol has no framing, no length prefix and no CRC, and the host
fixes its read length before it sends a command. A reply one byte short
desynchronises the stream and every later reply arrives shifted, so the
assertions here are about exact byte counts and exact layouts. The parsing side
is transcribed from FlashGBX's LK_Device.py and hw_GBFlash.py, not from local
headers.
"""

import ctypes
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CHECKS = 0
FAILS = []


def ck(cond, label, detail=""):
    global CHECKS
    CHECKS += 1
    if cond:
        return True
    FAILS.append(label)
    sys.stdout.write("  FAIL %s%s\n" % (label, ("  [%s]" % detail) if detail else ""))
    return False


def section(name):
    sys.stdout.write("%s\n" % name)


def build_lib(tmp):
    so = os.path.join(tmp, "libproto.so")
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-std=c99", "-g", "-O1", "-fPIC", "-shared",
           "-Wall", "-Wextra", "-Wno-unused-parameter",
           "-I", os.path.join(ROOT, "include"),
           "-o", so, os.path.join(ROOT, "src", "proto.c")]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode != 0:
        sys.stdout.write(p.stdout.decode("utf-8", "replace"))
        sys.exit("could not build src/proto.c for the host")
    return so


class Proto(object):

    # Hand-written mirror of fw_state_t. If the struct changes shape and this
    # does not, every state read lies. __init__ checks size and tail offset.
    class State(ctypes.Structure):
        _fields_ = [
            ("address", ctypes.c_uint32),
            ("transfer_size", ctypes.c_uint16),
            ("buffer_size", ctypes.c_uint16),
            ("dmg_rom_bank", ctypes.c_uint16),
            ("cart_mode", ctypes.c_uint8),
            ("pcb_ver", ctypes.c_uint8),
            ("dmg_access_mode", ctypes.c_uint8),
            ("dmg_read_method", ctypes.c_uint8),
            ("dmg_read_cs_pulse", ctypes.c_uint8),
            ("agb_read_method", ctypes.c_uint8),
            ("cart_powered", ctypes.c_uint8),
            ("pullups_enabled", ctypes.c_uint8),
            ("mode", ctypes.c_int),
            ("read_requested", ctypes.c_uint8),
            ("read_is_save", ctypes.c_uint8),
            ("read_is_eeprom", ctypes.c_uint8),
            ("read_is_3d", ctypes.c_uint8),
            ("page_terminator_due", ctypes.c_uint8),
            ("page_bytes_done", ctypes.c_uint16),
            ("page_end_requested", ctypes.c_uint8),
            ("rtc_read_requested", ctypes.c_uint8),
            ("bootup_requested", ctypes.c_uint8),
            ("eeprom_sel", ctypes.c_uint8),
            ("crc_requested", ctypes.c_uint8),
            ("crc_len", ctypes.c_uint32),
            ("save_write_requested", ctypes.c_uint8),
            ("save_write_is_dmg", ctypes.c_uint8),
            ("save_write_flash", ctypes.c_uint8),
            ("save_write_eeprom", ctypes.c_uint8),
            ("status_register", ctypes.c_uint16),
            ("last_bank_accessed", ctypes.c_uint16),
            ("status_reg_mask", ctypes.c_uint16),
            ("status_reg_value", ctypes.c_uint16),
            ("flash_we_pin_var", ctypes.c_uint8),
            ("we_pin_requested", ctypes.c_uint8),
            ("flash_pulse_reset", ctypes.c_uint8),
            ("flash_commands_bank_1", ctypes.c_uint8),
            ("flash_sharp_verify_sr", ctypes.c_uint8),
            ("flash_double_die", ctypes.c_uint8),
            ("audio_requested", ctypes.c_uint8),
            ("agb_irq_enabled", ctypes.c_uint8),
            ("dmg_audio_enabled", ctypes.c_uint8),
            ("dmg_bank_cmd_count", ctypes.c_uint8),
            ("dmg_bank_cmd_val", ctypes.c_uint32 * 8),
            ("dmg_bank_cmd_type", ctypes.c_uint8 * 8),
            ("save_write_len", ctypes.c_uint16),
            ("bench_requested", ctypes.c_uint8),
            ("power_requested", ctypes.c_uint8),
            ("power_target", ctypes.c_uint8),
            ("cart_step", ctypes.c_uint16),
            ("cart_latch", ctypes.c_uint16),
            ("cart_step_pin", ctypes.c_uint16),
            ("pullup_requested", ctypes.c_uint8),
            ("setpin_high", ctypes.c_uint8),
            ("setpin_mask", ctypes.c_uint32),
            ("tristate_requested", ctypes.c_uint8),
            ("voltage_requested", ctypes.c_uint8),
            ("voltage_five", ctypes.c_uint8),
            ("voltage_is_five", ctypes.c_uint8),
            ("write_requested", ctypes.c_uint8),
            ("write_addr", ctypes.c_uint32),
            ("write_value", ctypes.c_uint8),
            ("mbc_reset_requested", ctypes.c_uint8),
            ("debug_requested", ctypes.c_uint8),
            ("clk_requested", ctypes.c_uint8),
            ("clk_pulses", ctypes.c_uint32),
            ("dmg_write_cs_pulse", ctypes.c_uint8),
            ("flash_command_set", ctypes.c_uint8),
            ("flash_method", ctypes.c_uint8),
            ("flash_we_pin", ctypes.c_uint8),
            ("flash_cmd_addr", ctypes.c_uint32 * 6),
            ("flash_cmd_val", ctypes.c_uint16 * 6),
            ("batch_requested", ctypes.c_uint8),
            ("batch_flashcart", ctypes.c_uint8),
            ("batch_count", ctypes.c_uint8),
            ("batch_addr", ctypes.c_uint32 * 32),
            ("batch_val", ctypes.c_uint16 * 32),
            ("program_requested", ctypes.c_uint8),
            ("program_len", ctypes.c_uint16),
            ("program_done", ctypes.c_uint16),
            ("program_streamed", ctypes.c_uint16),
            ("usb_irq_state", ctypes.c_uint16),
            ("pump_stalls", ctypes.c_uint16),
            ("program_stream_err", ctypes.c_uint8),
            ("flash_write_requested", ctypes.c_uint8),
            ("flash_write_addr", ctypes.c_uint32),
            ("flash_write_val", ctypes.c_uint16),
            ("flash_write_is_agb", ctypes.c_uint8),
            ("reset_requested", ctypes.c_uint8),
        ]

    def __init__(self, so):
        self.lib = ctypes.CDLL(so)
        self.lib.fw_proto_state_size.restype = ctypes.c_uint32
        want = self.lib.fw_proto_state_size()
        got = ctypes.sizeof(Proto.State)
        if want != got:
            sys.exit("fw_state_t is %d bytes but the ctypes mirror in "
                     "test_proto.py is %d. Add the new field to Proto.State."
                     % (want, got))
        # Size alone is not enough: a uint8_t inserted before the last field
        # lands in existing padding, so sizeof does not move but every field
        # after it does.
        self.lib.fw_proto_state_tail_off.restype = ctypes.c_uint32
        want_off = self.lib.fw_proto_state_tail_off()
        got_off = Proto.State.reset_requested.offset
        if want_off != got_off:
            sys.exit("fw_state_t.reset_requested is at offset %d but the "
                     "ctypes mirror puts it at %d. A field was inserted "
                     "ahead of it. Update Proto.State." % (want_off, got_off))
        self.lib.fw_proto_feed.restype = ctypes.c_uint32
        self.lib.fw_proto_fw_info.restype = ctypes.c_uint32
        self.st = Proto.State()
        self.out = (ctypes.c_uint8 * 4096)()
        self.lib.fw_proto_init(ctypes.byref(self.st))

    def payload(self):
        self.lib.fw_proto_payload.restype = ctypes.POINTER(ctypes.c_uint8)
        return self.lib.fw_proto_payload()

    def send(self, data):
        if isinstance(data, int):
            data = bytes([data])
        reply = bytearray()
        for b in data:
            n = self.lib.fw_proto_feed(ctypes.byref(self.st),
                                       ctypes.c_uint8(b), self.out)
            if n:
                reply += bytes(self.out[:n])
        return bytes(reply)


CMD = {"NULL": 0x30, "DMG_CART_READ": 0xB1, "DMG_MBC_RESET": 0xB4, "QUERY_FW_INFO": 0xA1, "SET_MODE_AGB": 0xA2,
       "SET_MODE_DMG": 0xA3, "SET_VARIABLE": 0xA6, "AGB_CART_READ": 0xC1,
       "CART_PWR_ON": 0xF2, "CART_PWR_OFF": 0xF3, "QUERY_CART_PWR": 0xF4,
       "PING": 0xFE}

# (bit width, key), from LK_Device.DEVICE_VAR
VAR = {"ADDRESS": (32, 0x00), "TRANSFER_SIZE": (16, 0x00),
       "BUFFER_SIZE": (16, 0x01), "DMG_ROM_BANK": (16, 0x02),
       "CART_MODE": (8, 0x00), "AGB_READ_METHOD": (8, 0x0C),
       "CART_POWERED": (8, 0x0D), "LAST_BANK_ACCESSED": (16, 0x04)}


def set_variable(name, value):
    """Exactly LK_Device._set_fw_variable()'s buffer."""
    width, key = VAR[name]
    size = {8: 1, 16: 2, 32: 4}[width]
    return bytes([CMD["SET_VARIABLE"], size]) + struct.pack(">I", key) \
        + struct.pack(">I", value)


def parse_fw_info(reply):
    """Exactly hw_GBFlash.py LoadFirmwareVersion()'s read sequence."""
    size = reply[0]
    if size != 8:
        raise ValueError("length byte was %d, not 8" % size)
    data = reply[1:1 + 8]
    fw = {"cfw_id": chr(data[0]),
          "fw_ver": int.from_bytes(data[1:3], "big"),
          "pcb_ver": data[3],
          "fw_ts": int.from_bytes(data[4:8], "big")}
    rest = reply[9:]
    if fw["fw_ver"] >= 12:
        n = rest[0]
        fw["pcb_name"] = rest[1:1 + n].decode("ascii")
        fw["flags"] = (rest[1 + n], rest[2 + n])
        consumed = 9 + 1 + n + 2
    else:
        consumed = 9
    fw["_consumed"] = consumed
    return fw


def test_fw_info(p):
    section("QUERY_FW_INFO decodes the way FlashGBX parses it")
    reply = p.send(CMD["QUERY_FW_INFO"])
    ck(len(reply) > 0, "the device answers at all", "%d bytes" % len(reply))
    if not reply:
        return
    try:
        fw = parse_fw_info(reply)
    except Exception as e:
        ck(False, "the reply parses as a firmware info block", str(e))
        return
    ck(True, "the reply parses as a firmware info block")
    # A trailing byte starts the host's next read one byte late and shifts
    # every reply after it.
    ck(fw["_consumed"] == len(reply),
       "the host consumes exactly the bytes we sent",
       "consumed %d of %d" % (fw["_consumed"], len(reply)))
    ck(fw["cfw_id"] == "L", "cfw_id is 'L', which is what hw_GBFlash binds on",
       fw["cfw_id"])
    # >= 12 is what selects the ACKed SET_VARIABLE path in LK_Device.
    ck(fw["fw_ver"] >= 12,
       "fw_ver >= 12, so the host uses the modern protocol",
       str(fw["fw_ver"]))
    ck(fw.get("pcb_name"), "a PCB name is present, as fw_ver >= 12 requires",
       repr(fw.get("pcb_name")))
    ck("Open" in fw.get("pcb_name", ""),
       "the name distinguishes this from stock firmware",
       repr(fw.get("pcb_name")))


def test_set_variable_acks(p):
    section("SET_VARIABLE is acknowledged, once, per FlashGBX's wait_for_ack")
    for name, value in (("ADDRESS", 0x123456), ("TRANSFER_SIZE", 0x1000),
                        ("AGB_READ_METHOD", 2), ("CART_MODE", 1)):
        reply = p.send(set_variable(name, value))
        ck(len(reply) == 1, "%s replies with exactly one byte" % name,
           "%d bytes" % len(reply))
        ck(reply[:1] in (b"\x01", b"\x03"),
           "%s replies with an ACK wait_for_ack accepts" % name,
           repr(reply[:1]))


def test_size_selects_the_table(p):
    section("the SET_VARIABLE size byte selects the table, not just the width")
    # ADDRESS is (32, key 0) and TRANSFER_SIZE is (16, key 0): same key,
    # different size. Switching on the key alone aliases them, and the device
    # then reads the wrong address at the wrong length.
    p.send(set_variable("ADDRESS", 0xDEAD00))
    p.send(set_variable("TRANSFER_SIZE", 0x800))
    ck(p.st.address == 0xDEAD00, "ADDRESS kept its 32-bit value",
       "0x%X" % p.st.address)
    ck(p.st.transfer_size == 0x800, "TRANSFER_SIZE kept its own value",
       "0x%X" % p.st.transfer_size)

    p.send(set_variable("ADDRESS", 0))
    ck(p.st.transfer_size == 0x800,
       "setting ADDRESS did not disturb TRANSFER_SIZE",
       "0x%X" % p.st.transfer_size)


def fw_max_transfer():
    """FW_MAX_TRANSFER as fw_config.h defines it.

    Read rather than hard-coded: the requirement is that TRANSFER_SIZE is
    clamped to the size of `uint8_t g_reply[FW_MAX_TRANSFER]', not that it is
    0x1000.
    """
    hdr = open(os.path.join(ROOT, "include", "fw_config.h")).read()
    m = re.search(r"^#define\s+FW_MAX_TRANSFER\s+(0x[0-9A-Fa-f]+)u?\s*$",
                  hdr, re.M)
    if not m:
        sys.exit("test_proto: could not find FW_MAX_TRANSFER in fw_config.h")
    return int(m.group(1), 16)


def test_transfer_size_is_clamped(p):
    section("TRANSFER_SIZE is clamped to the reply buffer's size")
    # An unclamped value drives the read loop past the end of the reply buffer
    # and emits host-controlled bytes.
    cap = fw_max_transfer()
    p.send(set_variable("TRANSFER_SIZE", 0xFFFF))
    ck(p.st.transfer_size <= cap,
       "an oversized TRANSFER_SIZE is clamped to FW_MAX_TRANSFER (0x%X)" % cap,
       "0x%X" % p.st.transfer_size)


def test_modes_are_acked(p):
    section("mode changes are ACKed: the host waits on them at fw_ver >= 12")
    # LK_Device.SetMode() sends these as _write(cmd, wait=fw_ver >= 12) and
    # this firmware reports 12. Returning nothing costs a full timeout on every
    # mode change, and three wait_for_ack failures latch WRITE_DELAY on.
    for name, want in (("SET_MODE_AGB", 2), ("SET_MODE_DMG", 1)):
        reply = p.send(CMD[name])
        ck(len(reply) == 1 and reply[0] in (1, 3), "%s is ACKed" % name,
           repr(reply))
        ck(p.st.mode == want, "%s took effect" % name, str(p.st.mode))


def test_agb_mode_drops_the_rail_immediately(p):
    section("entering AGB mode selects 3.3 V without waiting to be asked")
    # SetMode() sends the mode first and the voltage second. Between them the
    # rail still carries the previous mode's selection, 5 V after DMG. A host
    # that dies in that gap leaves an AGB cartridge on 5 V.
    p.send(CMD["SET_MODE_DMG"])
    p.send(0xA5)                                   # rail asked to 5 V
    p.st.voltage_requested = 0
    p.send(CMD["SET_MODE_AGB"])
    ck(p.st.voltage_requested == 1,
       "SET_MODE_AGB requests a voltage change of its own accord")
    ck(p.st.voltage_five == 0, "and the direction is down, to 3.3 V")

    # Not symmetric: 5 V is the dangerous direction and is never selected
    # without an explicit request.
    p.st.voltage_requested = 0
    p.send(CMD["SET_MODE_DMG"])
    ck(p.st.voltage_requested == 0,
       "SET_MODE_DMG does not raise the rail by itself")


def test_five_volts_needs_dmg_mode(p):
    section("5 V is refused unless the device is positively in DMG mode")
    # A 3.3 V AGB cartridge on a 5 V rail is permanent damage.
    p.send(CMD["SET_MODE_AGB"])
    reply = p.send(0xA5)                        # SET_VOLTAGE_5V
    ck(len(reply) == 1, "the request is still ACKed", repr(reply))
    ck(p.st.voltage_five == 1, "and recorded as asked-for")

    p.lib.fw_proto_init(ctypes.byref(p.st))
    p.send(0xA5)
    ck(p.st.mode == 0, "no mode selected")

    p.send(CMD["SET_MODE_DMG"])
    reply = p.send(0xA5)
    ck(len(reply) == 1 and p.st.voltage_five == 1,
       "in DMG mode the same request is accepted")


def flash_cmd_buf(cmd_set, method, we_pin, pairs):
    """A7 <set> <method> <we> then 6 x (addr u32 BE, val u16 BE), zero-padded."""
    buf = bytes([0xA7, cmd_set, method, we_pin])
    for i in range(6):
        a, v = pairs[i] if i < len(pairs) else (0, 0)
        buf += struct.pack(">I", a) + struct.pack(">H", v)
    return buf


def test_unimplemented_vs_unknown(p):
    section("a deferred opcode swallows its whole command before replying")
    # A deferred opcode must not reply until its whole command has arrived.
    # Replying on the opcode resets the parser and the remaining bytes are read
    # as opcodes. On the save-restore path (LK_Device.py:1834-1836: opcode then
    # TRANSFER_SIZE raw bytes with nothing reading in between) that dispatches
    # 2048 bytes of the user's save file as commands: 0xB2/0xD1/0xD2 are
    # cartridge writes and 0xA5 is SET_VOLTAGE_5V, measured at 35 of 200 random
    # restores putting 5 V on a 3.3 V AGB cart. Command shapes transcribed from
    # LK_Device.py's call sites.
    #                            opcode  args  payload
    DEFERRED = [
        (0xB5, "DMG_MBC7_READ_EEPROM",      0, 0),
        (0xB6, "DMG_MBC7_WRITE_EEPROM",     0, "xfer"),
        (0xB7, "DMG_MBC6_MMSA_WRITE_FLASH", 0, "xfer"),
        (0xB9, "DMG_EEPROM_WRITE",          0, "xfer"),
        (0xBA, "DMG_CART_READ_MEASURE",     0, 0),
    ]
    XFER = 8
    p.send(set_variable("TRANSFER_SIZE", XFER))
    for op, name, nargs, pay in DEFERRED:
        # Argument bytes are 0xB8 and payload bytes 0xB2, both real opcodes
        # (0xB2 is DMG_CART_WRITE). A byte dispatched instead of consumed moves
        # write_requested.
        args = bytes([0xB8] * nargs)
        npay = XFER if pay == "xfer" else 0
        block = bytes([op]) + args + bytes([0xB2] * npay)
        p.st.write_requested = 0
        replies = [(i, p.send(bytes([b]))) for i, b in enumerate(block)]
        early = [(i, r) for i, r in replies[:-1] if r]
        ck(not early,
           "%s (0x%02X) says nothing until its last byte" % (name, op),
           "replied at %r" % (early,))
        ck(replies[-1][1] == b"\x02",
           "%s (0x%02X) then answers exactly one 0x02" % (name, op),
           repr(replies[-1][1]))
        ck(p.st.write_requested == 0,
           "%s (0x%02X) dispatches none of its own bytes" % (name, op))
        ck(p.send(b"\xfe\x5a") == b"\xa5",
           "%s (0x%02X) leaves the parser idle" % (name, op))
    for op in (0x7F, 0x42, 0xEE):
        reply = p.send(op)
        ck(reply == b"", "0x%02X (not an opcode) emits nothing" % op,
           repr(reply))
    # The resync byte keeps its 0x01: _try_write loops until it sees 1 or 2.
    reply = p.send(0x00)
    ck(reply == b"\x01", "0x00 still answers 0x01 for the resync loop",
       repr(reply))


def test_the_three_byte_five_volt_injection(p):
    section("the reported 5 V injection, byte for byte")
    # `C4 A3 A5`, ordinary content inside a .sav file. If AGB_CART_WRITE_SRAM
    # replies and resets the parser, 0xA3 becomes SET_MODE_DMG and 0xA5
    # SET_VOLTAGE_5V, and cart.c's `five_volt && dmg_mode` guard passes because
    # the injected stream has just satisfied it.
    p.send(CMD["SET_MODE_AGB"])
    p.send(set_variable("TRANSFER_SIZE", 8))
    p.st.voltage_requested = 0
    reply = p.send(bytes([0xC4, 0xA3, 0xA5]))
    ck(reply == b"", "the three bytes are the start of a payload, not commands",
       repr(reply))
    ck(p.st.voltage_five == 0 and p.st.voltage_requested == 0,
       "no voltage change is requested")
    ck(p.st.mode == 2, "and the cartridge mode is still AGB")
    # Finish the payload so the parser does not stay open into the next test.
    p.send(bytes([0x00] * 5))


def test_a_random_save_restore_dispatches_nothing(p):
    section("200 random save restores, the audit's own experiment")
    # WriteRAM's loop is
    #   _write(0xC4); _write(<TRANSFER_SIZE random bytes>, wait=True)
    # repeated over the file, with nothing reading in between. Measured at
    # 35/200 restores reaching fw_cart_voltage(five=1, dmg_mode=1), mean 3.5
    # stray cartridge writes each.
    import random
    rng = random.Random(20260817)
    XFER = 0x200
    p.send(CMD["SET_MODE_AGB"])
    p.send(set_variable("TRANSFER_SIZE", XFER))
    p.send(set_variable("ADDRESS", 0))
    dispatched = 0
    bad_reply = 0
    for _ in range(200):
        p.st.write_requested = 0
        p.st.voltage_requested = 0
        p.st.batch_requested = 0
        p.st.program_requested = 0
        p.st.reset_requested = 0
        p.st.flash_write_requested = 0
        p.st.save_write_requested = 0
        block = bytes(rng.getrandbits(8) for _ in range(XFER))
        reply = p.send(bytes([0xC4]) + block)
        if reply != b"\x01":
            bad_reply += 1
        if (p.st.write_requested or p.st.voltage_requested
                or p.st.batch_requested or p.st.program_requested
                or p.st.reset_requested or p.st.flash_write_requested):
            dispatched += 1
    ck(dispatched == 0,
       "no save block dispatches anything (audit measured 35/200 reaching 5 V)",
       "%d/200 dispatched" % dispatched)
    ck(bad_reply == 0, "and every one answers exactly one ACK byte",
       "%d/200 wrong" % bad_reply)
    ck(p.st.voltage_five == 0, "the rail select never moved")
    ck(p.st.mode == 2, "and the mode is still AGB")
    ck(p.send(b"\xfe\x5a") == b"\xa5", "the stream is in sync after 200 of them")


def test_an_abandoned_payload_times_out(p):
    section("a half-sent command does not outlive the host that sent it")
    # Nothing about a closed serial port reaches this layer. Without the
    # watchdog a half-collected FLASH_PROGRAM payload waits across the host
    # quitting, and the next session's QUERY_FW_INFO and handshake bytes fill
    # it and get programmed into the cartridge.
    def half_a_block():
        p.send(set_variable("TRANSFER_SIZE", 0x100))
        p.send(bytes([0xD3]) + bytes(0x40))      # 64 of 256 bytes, then silence

    # Probing with a PING is itself a byte, and a byte legitimately refreshes
    # the watchdog. Each case therefore needs its own fresh half-block.
    half_a_block()
    p.lib.fw_proto_tick(ctypes.byref(p.st), ctypes.c_uint32(1000))
    p.lib.fw_proto_tick(ctypes.byref(p.st), ctypes.c_uint32(1000 + 1999))
    ck(p.send(0xFE) == b"", "just under the timeout it is still collecting")
    p.lib.fw_proto_link_reset(ctypes.byref(p.st))

    half_a_block()
    p.lib.fw_proto_tick(ctypes.byref(p.st), ctypes.c_uint32(9000))
    p.lib.fw_proto_tick(ctypes.byref(p.st), ctypes.c_uint32(9000 + 2000))
    ck(p.send(b"\xfe\x5a") == b"\xa5", "past it the parser is abandoned and idle")
    ck(p.st.program_requested == 0, "and nothing was programmed")


def test_a_slow_but_live_host_is_not_abandoned(p):
    section("the timeout does not fire on a host that is merely slow")
    # SetVarState sleeps 200 ms between its opcode and its payload
    # (LK_Device.py:930-932). A timeout at or below that abandons a transfer
    # the host is still in the middle of.
    p.send(bytes([0xAF]) + bytes(32))            # half a var-state blob
    for t in range(0, 8000, 500):
        p.lib.fw_proto_tick(ctypes.byref(p.st), ctypes.c_uint32(t))
        p.send(bytes(1))
    ck(p.send(0xFE) == b"", "a byte every 500 ms keeps the command alive")
    p.send(bytes(64 - 32 - 16))                  # finish the blob
    ck(p.send(b"\xfe\x5a") == b"\xa5", "and it completes normally")


def test_a_usb_link_reset_abandons_the_parser(p):
    section("a USB bus reset drops whatever was half-sent")
    p.send(set_variable("TRANSFER_SIZE", 0x100))
    p.send(bytes([0xD3]) + bytes(0x40))
    p.lib.fw_proto_link_reset(ctypes.byref(p.st))
    ck(p.send(b"\xfe\x5a") == b"\xa5", "the next byte starts a new conversation")
    ck(p.st.program_requested == 0, "and nothing was programmed")


def test_save_memory_reads_and_writes(p):
    section("save memory: the last thing FlashGBX could not do at all")
    # Without these handlers DETECT_CART concludes a GBA cartridge has no save
    # memory and refuses a save restore.
    p.send(CMD["SET_MODE_AGB"])
    p.send(set_variable("TRANSFER_SIZE", 0x20))
    p.send(set_variable("ADDRESS", 0x40))
    p.st.read_requested = 0
    p.st.read_is_save = 0
    reply = p.send(0xC3)
    ck(reply == b"", "AGB_CART_READ_SRAM answers nothing from the dispatcher. "
       "The bytes come from the read loop, or the reply is sent twice",
       repr(reply))
    ck(p.st.read_requested == 1 and p.st.read_is_save == 1,
       "it asks for a SAVE read, not a ROM read at the same address")

    # WriteRAM's shape: opcode, then TRANSFER_SIZE raw bytes, then one ACK.
    p.st.save_write_requested = 0
    data = bytes(range(0x20))
    reply = p.send(bytes([0xC4]) + data)
    ck(reply == b"\x01", "AGB_CART_WRITE_SRAM ACKs once, after the block",
       repr(reply))
    ck(p.st.save_write_requested == 1 and p.st.save_write_is_dmg == 0,
       "and queues an AGB save write")
    ck(p.st.save_write_len == 0x20, "of the whole block",
       str(p.st.save_write_len))
    ck(bytes(p.payload()[:0x20]) == data, "with the data intact")

    p.send(CMD["SET_MODE_DMG"])
    p.st.save_write_requested = 0
    reply = p.send(bytes([0xB3]) + data)
    ck(reply == b"\x01", "DMG_CART_WRITE_SRAM ACKs the same way", repr(reply))
    ck(p.st.save_write_is_dmg == 1,
       "and takes the DMG path: the 0xA000 window on the ordinary bus, not "
       "a separate chip select")
    p.send(CMD["SET_MODE_AGB"])


def test_flash_save_write_is_not_a_plain_sram_write(p):
    section("AGB_CART_WRITE_FLASH_DATA carries the method, not just the data")
    # A FLASH save chip ignores a plain write: each byte needs the unlock
    # sequence in front of it. The host picks the method from the chip ID and
    # sends it as the one argument byte (LK_Device.py:3459-3460). Losing it
    # means a save restore that reports success and writes nothing.
    p.send(CMD["SET_MODE_AGB"])
    p.send(set_variable("TRANSFER_SIZE", 0x10))
    p.send(set_variable("ADDRESS", 0))
    p.st.save_write_requested = 0
    reply = p.send(bytes([0xC7, 0x01]) + bytes(range(0x10)))
    ck(reply == b"\x01", "it ACKs once, after the whole block", repr(reply))
    ck(p.st.save_write_requested == 1, "and queues a save write")
    ck(p.st.save_write_flash == 1,
       "carrying the method, so the unlock sequence is used",
       str(p.st.save_write_flash))
    ck(p.st.save_write_len == 0x10, "of the whole block")
    p.send(bytes([0xC4]) + bytes(0x10))
    ck(p.st.save_write_flash == 0,
       "while AGB_CART_WRITE_SRAM stays a plain write")


def test_the_flash_write_configuration_is_not_discarded(p):
    section("the whole flash-write configuration survives SET_VARIABLE")
    # A `default: break` in set_variable() drops these silently and leaves
    # every ROM write on the compiled-in defaults. The host reads several back,
    # so the loss is invisible until the write misbehaves.
    #                     name                      size  key    value
    cases = [("STATUS_REGISTER",           2, 0x03, 0x1234),
             ("LAST_BANK_ACCESSED",        2, 0x04, 0x0007),
             ("STATUS_REGISTER_MASK",      2, 0x05, 0xFFFF),
             ("STATUS_REGISTER_VALUE",     2, 0x06, 0x0080),
             ("FLASH_COMMAND_SET",         1, 0x02, 0x02),
             ("FLASH_METHOD",              1, 0x03, 0x02),
             ("FLASH_WE_PIN",              1, 0x04, 0x01),
             ("FLASH_PULSE_RESET",         1, 0x05, 0x01),
             ("FLASH_COMMANDS_BANK_1",     1, 0x06, 0x01),
             ("FLASH_SHARP_VERIFY_SR",     1, 0x07, 0x01),
             ("FLASH_DOUBLE_DIE",          1, 0x0A, 0x01),
             ("AGB_IRQ_ENABLED",           1, 0x10, 0x01),
             ("DMG_AUDIO_ENABLED",         1, 0x11, 0x01)]
    for name, size, key, val in cases:
        buf = bytes([0xA6, size]) + struct.pack(">I", key) + struct.pack(">I", val)
        reply = p.send(buf)
        ck(len(reply) == 1 and reply[0] in (1, 3), "%s is ACKed" % name, repr(reply))
        buf = bytes([0xAD, size]) + struct.pack(">I", key)
        got = p.send(buf)
        ck(len(got) == 4 and struct.unpack(">I", got)[0] == val,
           "%s reads back as it was set" % name,
           "wanted 0x%X, got %r" % (val, got))


def test_debug_is_acked(p):
    section("DEBUG is ACKed, because the host waits on it")
    # LK_Device.Debug() sends this with wait=True (:1058). Answering 0x02
    # counts a wait_for_ack failure, and three failures latch WRITE_DELAY on
    # for the rest of the session: a 1.4 ms sleep after every write. Stock
    # pulses CLK 20 times for a scope (0x9072, `cmp r0, #20`).
    p.st.debug_requested = 0
    reply = p.send(0xA0)
    ck(len(reply) == 1 and reply[0] in (1, 3), "DEBUG is ACKed", repr(reply))
    ck(p.st.debug_requested == 1, "and asks for the CLK pulses")


def test_bank_change_command_is_held(p):
    section("DMG_SET_BANK_CHANGE_CMD holds the sequence the host sends once")
    # A flash cart whose flash commands live on bank 1 needs this to select a
    # bank at all. The host sends it during setup and waits for an ACK
    # (LK_Device.py:4332-4353). A silent device costs a timeout there and the
    # sequence is then missing when a write needs it.
    pairs = [(0x2100, 0), (0x00AA, 1), (0x4000, 0)]
    buf = bytes([0xB8, len(pairs)])
    for v, t in pairs:
        buf += struct.pack(">I", v) + bytes([t])
    reply = p.send(buf)
    ck(reply == b"\x01", "the sequence is ACKed", repr(reply))
    ck(p.st.dmg_bank_cmd_count == 3, "all three entries are held",
       str(p.st.dmg_bank_cmd_count))
    ck(p.st.dmg_bank_cmd_val[0] == 0x2100 and p.st.dmg_bank_cmd_type[0] == 0,
       "an address entry keeps its type")
    ck(p.st.dmg_bank_cmd_val[1] == 0x00AA and p.st.dmg_bank_cmd_type[1] == 1,
       "and a value entry keeps its own")
    reply = p.send(bytes([0xB8, 0x00]))
    ck(reply == b"\x01", "a zero-length sequence is ACKed too", repr(reply))
    ck(p.st.dmg_bank_cmd_count == 0, "and clears what was held")
    # A partly applied bank switch selects the wrong bank and the write lands
    # there.
    buf = bytes([0xB8, 12]) + bytes(12 * 5)
    reply = p.send(buf)
    ck(reply == b"\x02", "an over-long sequence is refused", repr(reply))
    ck(p.send(b"\xfe\x5a") == b"\xa5", "and the stream stays in sync")


def test_the_boot_handshake_runs_only_on_the_agb_bus(p):
    section("AGB_BOOTUP_SEQUENCE is AGB-only")
    # The sequence is a run of AGB ROM reads. Against a DMG cartridge it would
    # drive that bus with cycles that have no meaning there.
    p.send(CMD["SET_MODE_AGB"])
    ck(p.send(bytes([0xC9])) == b"\x01", "AGB mode acknowledges")
    ck(p.st.bootup_requested == 1, "and queues the handshake")
    p.st.bootup_requested = 0
    p.send(CMD["SET_MODE_DMG"])
    ck(p.send(bytes([0xC9])) == b"\x01", "DMG mode acknowledges too")
    ck(p.st.bootup_requested == 0, "but drives nothing")
    p.send(CMD["SET_MODE_AGB"])


def test_the_rtc_read_is_deferred_but_its_length_is_not(p):
    section("AGB_READ_GPIO_RTC answers 8 bytes in either mode")
    # The host reads exactly 8 and then indexes them (Mapper.py:125-157). A
    # short reply makes _read return False, and `False[1:]` raises TypeError
    # out of ReadHeader on essentially every AGB cartridge.
    p.send(CMD["SET_MODE_AGB"])
    reply = p.send(bytes([0xCA]))
    ck(len(reply) == 8, "AGB mode still commits 8 bytes", "%d" % len(reply))
    ck(p.st.rtc_read_requested == 1, "and defers the read to the bus owner")
    p.st.rtc_read_requested = 0

    p.send(CMD["SET_MODE_DMG"])
    reply = p.send(bytes([0xCA]))
    ck(len(reply) == 8, "DMG mode answers 8 bytes too", "%d" % len(reply))
    ck(p.st.rtc_read_requested == 0,
       "but drives nothing: there are no GPIO registers on the DMG bus")
    ck(reply[0] == 0x80,
       "and says 'no clock' rather than inventing one", "0x%02X" % reply[0])
    p.send(CMD["SET_MODE_AGB"])


def test_the_3d_memory_page_terminator_is_swallowed(p):
    section("the bare 0x00 after every 3D Memory page is not an opcode")
    # ReadROM_3DMemory sends AGB_CART_READ_3D_MEMORY + read(length) eight times
    # then a bare self._write(0) to close the 0x1000-byte page
    # (LK_Device.py:1705-1716). Answering that zero, or treating it as an opcode
    # with arguments, misaligns every later reply.
    p.send(CMD["SET_MODE_AGB"])
    p.send(set_variable("TRANSFER_SIZE", 64))
    p.send(set_variable("BUFFER_SIZE", 0x1000))
    p.send(set_variable("ADDRESS", 0))
    p.send(bytes([0xC8]))
    ck(p.st.read_requested == 1 and p.st.read_is_3d == 1, "0xC8 queues a 3D read")
    p.st.read_requested = 0
    p.st.read_is_3d = 0
    reply = p.send(bytes([0x00]))
    ck(reply == b"", "the page terminator is answered with silence", repr(reply))
    ck(p.st.read_requested == 0, "and queues no further read")
    ck(p.st.page_end_requested == 1, "but does close the page")
    p.st.page_end_requested = 0
    ck(p.send(b"\xfe\x5a") == b"\xa5", "the next command still gets its own reply")
    reply = p.send(bytes([0x00]))
    ck(reply == b"\x01", "a 0x00 that is NOT a terminator still resyncs", repr(reply))


def test_the_intel_command_set_is_not_driven_as_amd(p):
    section("FLASH_METHOD 2 branches on the command set, AMD vs Intel/Sharp")
    # The six-slot table is AMD's. Intel and Sharp parts use a different
    # sequence in the same slots. The host says which via SET_FLASH_CMD's first
    # byte (0x01 AMD, 0x02 Intel/Sharp, LK_Device.py:4223-4234) but does not
    # gate method 2 on it (:4284). Twelve shipped AGB profiles are Intel/Sharp,
    # including M36L0R806 and 256L30B. Driven as AMD they never see 0xD0 or
    # 0xFF.
    p.send(CMD["SET_MODE_AGB"])
    p.send(flash_cmd_buf(0x02, 0x02, 0x01, [(0, 0xE8), (0, 0), (0, 0),
                                            (0, 0xD0), (0, 0xFF)]))
    ck(p.st.flash_command_set == 2, "the Intel command set is recorded",
       str(p.st.flash_command_set))
    ck(p.st.flash_method == 2, "and the buffered method")
    p.send(flash_cmd_buf(0x01, 0x02, 0x01, [(0xAAA >> 1, 0xAA), (0x555 >> 1, 0x55),
                                            (0, 0x25), (0, 0), (0, 0), (0, 0x29)]))
    ck(p.st.flash_command_set == 1, "and AMD is recorded distinctly")


def test_an_undriveable_flash_method_is_refused(p):
    section("a FLASH_METHOD we cannot drive is refused, not guessed at")
    # 3,4,5,8,9,0x0A,0x0B,0x0C are separate protocols. Falling through to the
    # single-write replay hands them a template that does not match. For GBAMP
    # the host assigns no commands at all, so the device would write bare data
    # words with no unlock and report success.
    p.send(CMD["SET_MODE_AGB"])
    p.send(set_variable("TRANSFER_SIZE", 16))
    p.send(set_variable("ADDRESS", 0))
    buf = bytes([0xA6, 1]) + struct.pack(">I", 0x03) + struct.pack(">I", 0x0B)
    p.send(buf)                       # FLASH_METHOD = 0x0B (GBAMP)
    ck(p.st.flash_method == 0x0B, "the method is recorded")
    reply = p.send(bytes([0xD3]) + bytes(16))
    ck(reply == b"\x02", "the FIRST block already answers ACK_ERROR", repr(reply))
    ck(p.st.program_requested == 0, "and no programming is queued")
    ck(p.send(b"\xfe\x5a") == b"\xa5", "and the stream stays in sync")


def test_eeprom_write_does_not_poison_the_next_flash_save(p):
    section("an EEPROM restore must not change how the NEXT save write behaves")
    # save_write_eeprom is written only by 0xC6 and 0xC4/0xB3, main.c tests it
    # before save_write_flash, and neither a cartridge power cycle nor a parser
    # reset clears it. Left set, the next FLASH save restore in the same USB
    # session takes the EEPROM branch and writes serial frames at a flash chip.
    p.send(CMD["SET_MODE_AGB"])
    p.send(set_variable("TRANSFER_SIZE", 8))
    p.send(bytes([0xC6, 0x01]) + bytes(8))          # an EEPROM write
    ck(p.st.save_write_eeprom == 1, "0xC6 selects the EEPROM path")
    p.send(bytes([0xC7, 0x01]) + bytes(8))          # then a FLASH-save write
    ck(p.st.save_write_eeprom == 0,
       "0xC7 clears it again, so the flash path is taken",
       str(p.st.save_write_eeprom))
    ck(p.st.save_write_flash == 1, "and the flash method is carried")
    p.send(bytes([0xC4]) + bytes(8))                # a plain SRAM write
    ck(p.st.save_write_eeprom == 0 and p.st.save_write_flash == 0,
       "and a plain SRAM write clears both")


def test_stream_progress_cleared_on_block_open(p):
    section("a new FLASH_PROGRAM block never inherits the last one's progress")
    # agb_stream_pump() in main.c tracks progress in program_done. Clearing it
    # in the pump's no-payload-open branch does not work: both call sites
    # require a payload already in progress, so a block abandoned mid-payload
    # leaves program_done set and the next block resumes from another
    # transfer's offset. `filled - program_done >= chunk` then underflows,
    # measured at ~2017 iterations instead of 1: ~63 KB read past a 2048-byte
    # payload buffer and programmed across 64 KB of cartridge address space.
    # proto.c clears it on the way in, the one path every block takes. main.c
    # is not compiled here, so the pump is simulated by setting the counter.
    p.send(CMD["SET_MODE_AGB"])
    p.send(set_variable("TRANSFER_SIZE", 64))

    p.send(bytes([0xD3]) + bytes(32))
    p.st.program_done = 32              # what the pump would have reached
    p.st.program_stream_err = 1
    ck(p.st.program_done == 32, "a part-received block can leave progress set")

    # parser_reset() knows nothing about fw_state_t and cannot clear these.
    p.lib.fw_proto_tick(ctypes.byref(p.st), ctypes.c_uint32(1000))
    p.lib.fw_proto_tick(ctypes.byref(p.st), ctypes.c_uint32(1000 + 2001))

    # The opcode alone declares the payload, so the counter must be clear
    # before a single payload byte is accepted.
    p.send(bytes([0xD3]))
    ck(p.st.program_done == 0,
       "opening a FLASH_PROGRAM payload clears program_done",
       "still %d. The pump would resume mid-block and its loop condition "
       "would underflow" % p.st.program_done)
    ck(p.st.program_stream_err == 0,
       "and clears the carried-forward error, so a stale failure cannot fail "
       "an unrelated block", str(p.st.program_stream_err))

    p.send(bytes(64))                   # finish this block
    p.st.program_done = 64
    p.send(bytes([0xD3]))
    ck(p.st.program_done == 0,
       "and it is cleared for a normal block too, not just an abandoned one",
       str(p.st.program_done))


def test_3d_page_terminator_survives_the_automatic_close(p):
    section("a 3D Memory page terminator is still swallowed after an auto-close")
    # The 0xC8 handler sets page_terminator_due so the bare 0x00 that closes a
    # page is swallowed rather than ACKed: an ACK puts a stray 0x01 in the
    # host's input buffer, which it reads as the first byte of the next
    # 512-byte read, shifting every page of a 3D Memory dump by one.
    #
    # main.c also closes the page automatically once page_bytes_done reaches
    # buffer_size. Do not clear the flag in do_3d_page_end(): on the last read
    # of every page that close consumes it first and the terminator is ACKed.
    # main.c is not compiled into this suite, so the source grep below is the
    # only check here that fails when the defect comes back.
    src_main = open(os.path.join(HERE, "..", "src", "main.c")).read()
    src_proto = open(os.path.join(HERE, "..", "src", "proto.c")).read()
    clears_main = len(re.findall(r"page_terminator_due\s*=\s*0", src_main))
    clears_proto = len(re.findall(r"page_terminator_due\s*=\s*0", src_proto))
    ck(clears_main == 0,
       "main.c never clears page_terminator_due",
       "%d assignment(s): do_3d_page_end() has two callers and only one of "
       "them is the host's terminator, so clearing there ACKs the terminator "
       "on every filled page" % clears_main)
    ck(clears_proto == 1,
       "and proto.c clears it in exactly one place, execute()",
       "%d assignment(s)" % clears_proto)

    # Both halves matter: recovery breaks if a real resync stops being
    # answered.
    p.send(CMD["SET_MODE_AGB"])
    p.send(bytes([0xC8]))
    p.st.read_requested = 0
    p.st.read_is_3d = 0
    ck(p.send(bytes([0x00])) == b"",
       "a 0x00 straight after a 3D read is swallowed, not answered")
    p.send(CMD["QUERY_FW_INFO"])
    ck(p.send(bytes([0x00])) == b"\x01",
       "and a 0x00 that does not follow a 3D read is still a resync, and is ACKed")


def test_dmg_rom_bank_mirrors_into_last_bank_accessed(p):
    section("setting DMG_ROM_BANK also updates LAST_BANK_ACCESSED")
    # The bank-1 unlock excursion (nine shipped profiles carry
    # "flash_commands_on_bank_1") selects bank 1 for the AAA/555 unlock, then
    # restores the working bank from last_bank_accessed. The host never sets
    # that variable; it sets DMG_ROM_BANK before every bank of a ROM write
    # (LK_Device.py:4587, :4669), and LK.c:252-254 mirrors it into
    # LAST_BANK_ACCESSED on the way through.
    #
    # Without the mirror the restore writes 0, banks 2..N are programmed into
    # bank 0's mapping, and the per-byte poll reads back in that same wrong
    # bank and matches: a whole ROM in one bank, returned as ACK_OK.
    p.send(CMD["SET_MODE_DMG"])
    for bank in (1, 2, 0x7F, 0x100 | 0x2A):
        p.send(set_variable("DMG_ROM_BANK", bank))
        ck(p.st.dmg_rom_bank == bank,
           "DMG_ROM_BANK %d is stored" % bank, str(p.st.dmg_rom_bank))
        ck(p.st.last_bank_accessed == (bank & 0xFF),
           "and LAST_BANK_ACCESSED follows it as %d" % (bank & 0xFF),
           "got %d. The bank-1 excursion would restore the wrong bank"
           % p.st.last_bank_accessed)

    p.send(set_variable("LAST_BANK_ACCESSED", 0x11))
    ck(p.st.last_bank_accessed == 0x11,
       "and an explicit LAST_BANK_ACCESSED still takes effect")


def test_agb_cart_write_and_crc32(p):
    section("AGB_CART_WRITE and CALC_CRC32: the two that cost timeouts")
    # 0xC2 is what _cart_write sends on AGB whenever flashcart is not set
    # (LK_Device.py:709-713): every Mapper write on AGB, including GPIO/RTC.
    # Reply with exactly one byte, which is all the host reads.
    p.send(CMD["SET_MODE_AGB"])
    p.st.flash_write_requested = 0
    reply = p.send(bytes([0xC2]) + struct.pack(">I", 0x0400_0000 >> 1)
                   + struct.pack(">H", 0x00A5))
    ck(reply == b"\x01", "AGB_CART_WRITE is ACKed once", repr(reply))
    ck(p.st.flash_write_requested == 1 and p.st.flash_write_is_agb == 1,
       "and queues an AGB bus write")
    ck(p.st.flash_write_val == 0x00A5, "with the value intact")

    # 0xD5's shape is _read(4) and nothing else (LK_Device.py:2269-2271). No
    # wait_for_ack on this path: a fifth ACK byte desynchronises the stream.
    # Stock emits four single-byte transmits (0x75A0-0x75C0) and stops.
    p.st.crc_requested = 0
    reply = p.send(bytes([0xD5]) + struct.pack(">I", 0x8000))
    ck(reply == b"", "CALC_CRC32 answers nothing from the dispatcher. The "
       "five bytes come from the bus walk", repr(reply))
    ck(p.st.crc_requested == 1 and p.st.crc_len == 0x8000,
       "and asks for the length the host gave", hex(p.st.crc_len))


def test_var_state_round_trips(p):
    section("GET_VAR_STATE and SET_VAR_STATE are a matched pair")
    # The host grabs the state, asks for a physical re-plug, then restores it
    # (LK_Device.py:793-819). GetVarState reads whatever in_waiting holds, so
    # the device sets the length on both sides, and SetVarState sends no length
    # prefix. A mismatch desynchronises.
    p.send(set_variable("ADDRESS", 0x123456))
    p.send(set_variable("TRANSFER_SIZE", 0x200))
    p.send(flash_cmd_buf(1, 2, 3, [(0xAAA, 0xA9), (0x555, 0x56)]))
    saved = p.send(0xAE)
    ck(len(saved) == 64, "GET_VAR_STATE answers a fixed 64 bytes", len(saved))
    p.send(set_variable("ADDRESS", 0))
    p.send(set_variable("TRANSFER_SIZE", 1))
    p.send(flash_cmd_buf(0, 0, 0, []))
    reply = p.send(bytes([0xAF]) + saved)
    ck(reply == b"\x01", "SET_VAR_STATE acks, as fw_ver 15 requires", repr(reply))
    ck(p.st.address == 0x123456, "the address came back", hex(p.st.address))
    ck(p.st.transfer_size == 0x200, "the transfer size came back")
    ck(p.st.flash_command_set == 1 and p.st.flash_cmd_addr[0] == 0xAAA
       and p.st.flash_cmd_val[0] == 0xA9,
       "and so did the flash command set, the thing it exists for")
    ck(p.send(b"\xfe\x5a") == b"\xa5", "the stream is in sync afterwards")


def test_var_state_never_restores_the_rail(p):
    section("SET_VAR_STATE cannot raise the cartridge voltage")
    # A blob is host-supplied bytes. Round-tripping the voltage select through
    # it is a route to 5 V that never passes SET_VOLTAGE_5V's dmg_mode guard.
    p.send(CMD["SET_MODE_DMG"])
    p.send(0xA5)
    ck(p.st.voltage_five == 1, "5 V is selected in DMG mode")
    blob = p.send(0xAE)
    p.send(CMD["SET_MODE_AGB"])
    ck(p.st.voltage_five == 0, "SET_MODE_AGB forces it back down")
    p.st.voltage_requested = 0
    p.send(bytes([0xAF]) + blob)
    ck(p.st.voltage_five == 0 and p.st.voltage_requested == 0,
       "and restoring the blob does not bring it back")
    ck(p.st.cart_powered == 0, "cart power is not restored from a blob either")


def test_resync_is_answered(p):
    section("the host's resync byte is answered, or recovery never completes")
    # LK_Device._try_write()'s recovery loop writes a bare 0x00 and reads one
    # byte, looping until it sees 1 or 2, twenty times before giving up on the
    # device. 0x00 is not DEVICE_CMD["NULL"] (0x30), so a dispatcher that knows
    # only the opcode table stays silent and error recovery is unreachable.
    for op, name in ((0x00, "the bare resync byte"), (0x30, "DEVICE_CMD NULL")):
        reply = p.send(op)
        ck(len(reply) == 1, "%s gets exactly one byte" % name,
           "%d" % len(reply))
        ck(reply[:1] in (b"\x01", b"\x02"),
           "%s gets a value the recovery loop accepts" % name, repr(reply[:1]))


def test_stream_is_position_independent(p):
    section("the parser does not care how the stream is chunked")
    # USB delivers in packets of 32 and commands straddle them.
    whole = p.send(set_variable("ADDRESS", 0xABCD))
    p.lib.fw_proto_init(ctypes.byref(p.st))
    buf = set_variable("ADDRESS", 0xABCD)
    split = bytearray()
    for b in buf:
        split += p.send(bytes([b]))
    ck(bytes(split) == whole,
       "one byte at a time gives the same reply as all at once",
       "%r vs %r" % (bytes(split), whole))
    ck(p.st.address == 0xABCD, "and the same effect", "0x%X" % p.st.address)


def test_a_realistic_session(p):
    section("a realistic FlashGBX opening sequence stays in step")
    p.lib.fw_proto_init(ctypes.byref(p.st))
    total = bytearray()

    reply = p.send(CMD["QUERY_FW_INFO"])
    fw = parse_fw_info(reply)
    total += reply
    ck(fw["_consumed"] == len(reply), "identity consumed exactly")

    for chunk, expect in (
            (bytes([CMD["SET_MODE_AGB"]]), 1),   # ACKed at fw_ver >= 12
            (set_variable("TRANSFER_SIZE", 0x1000), 1),
            (set_variable("ADDRESS", 0), 1),
            (set_variable("AGB_READ_METHOD", 2), 1),
    ):
        r = p.send(chunk)
        ck(len(r) == expect,
           "%02X... replies with %d byte(s)" % (chunk[0], expect),
           "%d" % len(r))

    ck(p.st.mode == 2, "mode is AGB")
    ck(p.st.transfer_size == 0x1000, "transfer size is the host's maximum")
    ck(p.st.agb_read_method == 2, "read method took")


def test_capability_flags(p):
    section("the capability flags FlashGBX gates features on")
    fw = parse_fw_info(p.send(CMD["QUERY_FW_INFO"]))
    f1, f2 = fw["flags"]
    # hw_GBFlash.LoadFirmwareVersion() decodes these bit by bit, and
    # SupportsBootloaderReset() returns FW["bootloader_reset"]. With bit 0 of
    # flag 2 clear, BootloaderReset() refuses to run and the only way back into
    # update mode is U22 held at power-on.
    ck(f2 & 0x01, "bootloader_reset is advertised, so FlashGBX will offer it",
       "flag2=0x%02X" % f2)
    # Bit 7 of flag 2 is the host's "unregistered" clone warning.
    ck(not (f2 & 0x80), "the unregistered/clone bit is clear",
       "flag2=0x%02X" % f2)
    ck(f1 & 0x01, "cart power control is claimed. Without it CanPowerCycleCart() "
       "is False and the host never energises the slot, so every dump is open bus",
       "flag1=0x%02X" % f1)


def test_bootloader_reset_sequence(p):
    section("BOOTLOADER_RESET fires on the opcode, ACK first")
    # hw_GBFlash.BootloaderReset() reads the ACK, sends a confirm byte, then
    # closes the port without reading anything else. Resetting on the first
    # byte is invisible to it provided the ACK is on the wire first.
    #
    # Do not arm on the opcode and fire on the confirm: that makes the recovery
    # command depend on state surviving between two USB packets, which passes
    # here and fails on hardware, leaving the device un-reflashable over USB.
    reply = p.send(0xF1)
    ck(len(reply) == 1 and reply[0] in (1, 3),
       "the opcode is ACKed", repr(reply))
    ck(p.st.reset_requested == 1,
       "and the reset is requested immediately, no second byte needed")


def test_stray_confirm_is_harmless(p):
    section("the host's trailing confirm byte is harmless")
    p.send(0xF1)
    # FlashGBX still sends 0x01 after the opcode. The reset is already under
    # way; if the byte is seen at all it is an unknown opcode and draws no
    # reply.
    reply = p.send(0x01)
    ck(reply == b"", "a trailing 0x01 emits nothing", repr(reply))


def test_agb_read_signals_rather_than_replies(p):
    section("AGB_CART_READ signals the read; it does not answer from here")
    p.send(set_variable("TRANSFER_SIZE", 0x1000))
    p.send(set_variable("ADDRESS", 0x8000))
    reply = p.send(CMD["AGB_CART_READ"])
    # main.c drives the bus and sends exactly transfer_size bytes. A byte from
    # here prefixes the payload and shifts the whole dump by one.
    ck(reply == b"", "the opcode itself emits no bytes", repr(reply))
    ck(p.st.read_requested == 1, "and the read is requested")


def test_power_reports_hardware_not_intent(p):
    section("cart power reports the rail, not the request")
    # Do not set cart_powered in the dispatcher. With PB22 not actually driven
    # the host is told the cartridge is powered when it is not and reads open
    # bus, which looks exactly like cartridge data: a wrong dump, not an error.
    reply = p.send(CMD["CART_PWR_ON"])
    ck(len(reply) == 1 and reply[0] in (1, 3), "CART_PWR_ON is ACKed", repr(reply))
    ck(p.st.power_requested == 1, "and the hardware change is requested")
    ck(p.st.power_target == 1, "with the right target")
    ck(p.st.cart_powered == 0,
       "cart_powered is NOT set by the dispatcher. main.c sets it after the "
       "rail has actually moved")

    p.st.power_requested = 0
    reply = p.send(CMD["CART_PWR_OFF"])
    ck(p.st.power_requested == 1 and p.st.power_target == 0,
       "CART_PWR_OFF requests the other direction")


def get_variable(name):
    """Exactly LK_Device._get_fw_variable()'s buffer."""
    width, key = VAR[name]
    size = {8: 1, 16: 2, 32: 4}[width]
    return bytes([0xAD, size]) + struct.pack(">I", key)


def test_get_variable_always_four_bytes(p):
    section("GET_VARIABLE answers four bytes whatever the variable's width")
    # _get_fw_variable does _read(4) and unpacks ">I" regardless of the
    # declared size. An 8-bit variable answered in one byte makes three bytes
    # of the next reply read as this one's.
    p.send(set_variable("ADDRESS", 0x123456))
    p.send(set_variable("TRANSFER_SIZE", 0x0800))
    p.send(set_variable("AGB_READ_METHOD", 2))

    for name, want in (("ADDRESS", 0x123456), ("TRANSFER_SIZE", 0x0800),
                       ("AGB_READ_METHOD", 2)):
        reply = p.send(get_variable(name))
        ck(len(reply) == 4, "%s answers exactly 4 bytes" % name,
           "%d bytes" % len(reply))
        if len(reply) == 4:
            got = struct.unpack(">I", reply)[0]
            ck(got == want, "%s round-trips its value" % name,
               "0x%X vs 0x%X" % (got, want))

    # Silence on an unknown key desynchronises the stream.
    reply = p.send(bytes([0xAD, 1]) + struct.pack(">I", 0x7F))
    ck(len(reply) == 4, "an unknown key still answers 4 bytes",
       "%d bytes" % len(reply))


def test_agb_header_commands_do_not_crash_the_host(p):
    section("the AGB header path: the commands that stop FlashGBX dying")
    # AGB_GPIO.HasRTC (Mapper.py:1863) does `if buffer is not None:
    # self.RTC_BUFFER = buffer[1:]`. A silent 0xCA makes _read return False and
    # `False[1:]` tracebacks out of ReadHeader.
    reply = p.send(0xCA)
    ck(len(reply) == 8, "AGB_READ_GPIO_RTC answers 8 bytes", "%d" % len(reply))
    ck(reply[:1] == b"\x80",
       "with bit 7 set, so HasRTC short-circuits to 'no RTC' honestly",
       repr(reply[:1]))

    reply = p.send(0xC9)
    ck(len(reply) == 1, "AGB_BOOTUP_SEQUENCE is ACKed", repr(reply))

    for op, name in ((0xAB, "ENABLE_PULLUPS"), (0xAC, "DISABLE_PULLUPS"),
                     (0xA8, "SET_ADDR_AS_INPUTS")):
        reply = p.send(op)
        ck(len(reply) == 1 and reply[0] in (1, 3), "%s is ACKed" % name,
           repr(reply))


def test_set_pin_only_touches_power(p):
    section("SET_PIN honours the rail bit and ignores the bus lines")
    # Bit 0 is CART_POWER; bits 1..30 are individual bus lines. A host-driven
    # /WR is a write strobe with no address phase behind it, and there is no
    # state here to tell a mask ROM from a flash cart.
    reply = p.send(bytes([0xF5]) + struct.pack(">I", 0x1) + bytes([1]))
    ck(len(reply) == 1, "SET_PIN replies with exactly one byte", repr(reply))
    ck(p.st.power_requested == 1 and p.st.power_target == 1,
       "bit 0 set high requests the rail on")

    p.st.power_requested = 0
    # /WR (bit 2) and /RD (bit 3).
    reply = p.send(bytes([0xF5]) + struct.pack(">I", 0xC) + bytes([0]))
    ck(len(reply) == 1, "a bus-line SET_PIN still gets its ACK", repr(reply))
    ck(p.st.power_requested == 0,
       "and does not touch the rail")


def test_dmg_bank_switch_is_a_plain_write(p):
    section("DMG_CART_WRITE: the whole of MBC support")
    # Mapper.py has a class per mapper, each overriding SelectBankROM to build
    # [address, value] pairs that CartWrite() sends as plain 0xB2 writes. The
    # firmware needs no knowledge of any of them.
    p.send(CMD["SET_MODE_DMG"])
    # LK_Device._cart_write DMG non-SRAM: B2 <addr u32 BE> <value u8>
    reply = p.send(bytes([0xB2]) + struct.pack(">I", 0x2100) + bytes([0x05]))
    ck(len(reply) == 1 and reply[0] in (1, 3), "the write is ACKed", repr(reply))
    ck(p.st.write_requested == 1, "and requested")
    ck(p.st.write_addr == 0x2100, "at the address the host sent",
       "0x%X" % p.st.write_addr)
    ck(p.st.write_value == 0x05, "with the value the host sent",
       "0x%X" % p.st.write_value)


def test_dmg_read_advances_by_bytes(p):
    section("DMG addressing is byte-wise, not halfword-wise like AGB")
    p.send(CMD["SET_MODE_DMG"])
    p.send(set_variable("TRANSFER_SIZE", 0x40))
    p.send(set_variable("ADDRESS", 0x4000))
    reply = p.send(CMD["DMG_CART_READ"])
    ck(reply == b"", "DMG_CART_READ signals rather than answering here",
       repr(reply))
    ck(p.st.read_requested == 1, "and the read is requested")
    # The AGB path advances address by len/2 because AGB addresses are halfword
    # indices. Doing that on DMG would dump half the cartridge, twice.
    ck(p.st.address == 0x4000,
       "the dispatcher does not move ADDRESS. main.c does, by BYTES on DMG")


def test_clk_toggle_and_mbc_reset(p):
    section("CLK_TOGGLE and DMG_MBC_RESET")
    # _clk_toggle sends A9 <count u32 BE> and waits for an ACK. MBC3+RTC
    # detection uses it, so every MBC3 header read depends on it.
    reply = p.send(bytes([0xA9]) + struct.pack(">I", 0x3C))
    ck(len(reply) == 1 and reply[0] in (1, 3), "CLK_TOGGLE is ACKed",
       repr(reply))
    ck(p.st.clk_pulses == 0x3C, "with the pulse count the host asked for",
       str(p.st.clk_pulses))

    reply = p.send(CMD.get("DMG_MBC_RESET", 0xB4))
    ck(len(reply) == 1 and reply[0] in (1, 3), "DMG_MBC_RESET is ACKed",
       repr(reply))
    ck(p.st.mbc_reset_requested == 1, "and requested")


def test_set_flash_cmd_carries_the_chip_sequence(p):
    section("SET_FLASH_CMD: the host teaches the device the chip")
    # The host waits for an ACK on this at fw_ver >= 12. The AMD unlock for a
    # typical cart is 5555/AA, 2AAA/55, 5555/A0.
    cmds = [(0x5555, 0xAA), (0x2AAA, 0x55), (0x5555, 0xA0)]
    buf = bytes([0xA7, 0x00, 0x01, 0x01])
    for i in range(6):
        a, v = cmds[i] if i < len(cmds) else (0, 0)
        buf += struct.pack(">I", a) + struct.pack(">H", v)
    reply = p.send(buf)
    ck(len(reply) == 1 and reply[0] in (1, 3), "SET_FLASH_CMD is ACKed",
       repr(reply))
    ck(p.st.flash_method == 1, "the method is stored", str(p.st.flash_method))
    ck(p.st.flash_cmd_addr[0] == 0x5555 and p.st.flash_cmd_val[0] == 0xAA,
       "and the first unlock pair")
    ck(p.st.flash_cmd_addr[2] == 0x5555 and p.st.flash_cmd_val[2] == 0xA0,
       "and the program command")
    ck(p.st.flash_cmd_addr[3] == 0 and p.st.flash_cmd_val[3] == 0,
       "with the unused slots zeroed, as the host pads them")


def test_cart_write_flash_cmd_is_variable_length(p):
    section("CART_WRITE_FLASH_CMD: a batch whose length is on the wire")
    # D4 <flashcart u8> <count u8> then count x (addr u32 BE, val u16 BE).
    # The count arrives from the host, so the parser has to read the header
    # before it knows how much more to expect.
    pairs = [(0x5555, 0xAA), (0x2AAA, 0x55), (0x5555, 0x80)]
    buf = bytes([0xD4, 0x01, len(pairs)])
    for a, v in pairs:
        buf += struct.pack(">I", a) + struct.pack(">H", v)
    reply = p.send(buf)
    ck(len(reply) == 1 and reply[0] in (1, 3), "the batch is ACKed",
       repr(reply))
    ck(p.st.batch_count == 3, "all three pairs arrived",
       str(p.st.batch_count))
    ck(p.st.batch_addr[2] == 0x5555 and p.st.batch_val[2] == 0x80,
       "including the last one")
    ck(p.st.batch_flashcart == 1, "and the flashcart flag")


def test_set_step_pins_and_unpins(p):
    section("SET_STEP: an argument of zero stops overriding, it is not 32")
    # 0xE1 00 releases the pin; it does not install the compiled-in 32. The
    # boot step follows the bulk packet size, and do_cart_read() picks a step
    # per read method unless the host has pinned one. A reset that installs a
    # constant defeats both, and a sweep's own tidy-up line is where that
    # happens: measure, "restore", then measure everything else at 32.
    p.lib.fw_proto_init(ctypes.byref(p.st))
    base = p.st.cart_step
    ck(p.st.cart_step_pin == 0, "nothing is pinned after init")

    reply = p.send(bytes([0xE1, 8]))
    ck(len(reply) == 1 and reply[0] == 1, "SET_STEP is ACKed", repr(reply))
    ck(p.st.cart_step_pin == 128, "8 x 16 = 128 bytes",
       str(p.st.cart_step_pin))
    ck(p.st.cart_step == base,
       "and the base main.c owns is left alone", str(p.st.cart_step))

    p.send(bytes([0xE1, 0]))
    ck(p.st.cart_step_pin == 0, "zero releases it")
    ck(p.st.cart_step == base,
       "and the base is still what it was before the sweep touched anything",
       "%d, expected %d" % (p.st.cart_step, base))


def test_flash_program_payload(p):
    section("FLASH_PROGRAM: a payload with no length on the wire")
    # The host sends the opcode then exactly TRANSFER_SIZE bytes, having set
    # that variable beforehand. Nothing in the stream says how long it is.
    p.send(CMD["SET_MODE_AGB"])
    p.send(set_variable("TRANSFER_SIZE", 64))
    p.send(set_variable("ADDRESS", 0x1000))
    reply = p.send(bytes([0xD3]))
    ck(reply == b"", "the opcode alone draws no reply, data is expected",
       repr(reply))
    reply = p.send(bytes(range(64)))
    ck(len(reply) == 1 and reply[0] in (1, 3),
       "the block is ACKed once the payload is complete", repr(reply))
    ck(p.st.program_requested == 1, "and programming is requested")
    ck(p.st.program_len == 64, "with the right length",
       str(p.st.program_len))


def test_a_program_block_split_across_packets(p):
    section("a FLASH_PROGRAM payload split across USB packets still lands")
    p.send(CMD["SET_MODE_AGB"])
    p.send(set_variable("TRANSFER_SIZE", 96))
    p.send(bytes([0xD3]))
    data = bytes((i * 7) & 0xFF for i in range(96))
    got = bytearray()
    for i in range(0, 96, 13):          # deliberately not a packet multiple
        got += p.send(data[i:i + 13])
    ck(len(got) == 1 and got[0] in (1, 3),
       "one ACK, after the last byte and not before", repr(bytes(got)))
    ck(p.st.program_len == 96, "the whole payload was collected",
       str(p.st.program_len))


def test_an_oversized_payload_is_consumed_not_truncated(p):
    section("an over-long payload is swallowed whole, then refused")
    # TRANSFER_SIZE reaches 0x1000 in normal use (the host's MAX_BUFFER_READ)
    # while the payload buffer is 0x800. Ending the command at 0x800 leaves the
    # remaining 0x800 bytes parsed as opcodes, and for FLASH_PROGRAM those
    # bytes are ROM data containing 0xB2 / 0xD1 / 0xD2, all cartridge writes.
    p.send(CMD["SET_MODE_AGB"])
    p.send(set_variable("TRANSFER_SIZE", 0x1000))
    p.send(bytes([0xD3]))
    data = bytes((i * 3) & 0xFF for i in range(0x1000))
    reply = p.send(data)
    ck(reply == b"\x02",
       "exactly one error byte after the whole payload, and nothing else",
       repr(reply[:8]))
    ck(p.st.program_requested == 0,
       "and programming is NOT started from a partly-received block")
    reply = p.send(b"\xfe\x5a")
    ck(reply == b"\xa5", "the stream is still in sync afterwards", repr(reply))


def test_ping_answers_the_challenge(p):
    section("PING answers a challenge, and consumes exactly one argument byte")
    # From fw_ver 15 CheckActive() sends FE <challenge> and expects the
    # complement (LK_Device.py:250-256). It runs on a timer, so a wrong answer
    # takes down every operation rather than one.
    for ch in (0x00, 0x01, 0x5A, 0x7F, 0x80, 0xFE, 0xFF):
        reply = p.send(bytes([0xFE, ch]))
        ck(reply == bytes([(~ch) & 0xFF]),
           "PING 0x%02X answers 0x%02X" % (ch, (~ch) & 0xFF), repr(reply))
    # A bare 0xFE must not answer: the next byte is its argument, and answering
    # early would put a spurious byte in front of the next command's reply.
    ck(p.send(0xFE) == b"", "a bare PING waits for its challenge")
    ck(p.send(0x5A) == b"\xa5", "and the next byte completes it")


def test_switch_state_answers_zero(p):
    section("GET_SWITCH_STATE answers a constant zero, as stock does")
    # No presence or mode switch exists on this board and neither capability
    # bit is advertised, so the host never asks. Answering matches stock rather
    # than leaving a known opcode with no reply behind it.
    reply = p.send(0xF6)
    ck(reply == b"\x00", "GET_SWITCH_STATE answers one zero byte", repr(reply))
    ck(p.send(b"\xfe\x5a") == b"\xa5", "and the stream stays in sync")


def test_an_oversized_batch_is_refused_not_clipped(p):
    section("an over-long flash-command batch is refused, not partly applied")
    # A partly applied unlock sequence leaves the chip in an undefined state,
    # and the host, told the batch succeeded, goes on to program it.
    n = 40
    buf = bytes([0xD4, 0x01, n])
    for i in range(n):
        buf += struct.pack(">I", 0x5555) + struct.pack(">H", 0xAA)
    reply = p.send(buf)
    ck(reply == b"\x02", "the batch is refused with an error", repr(reply))
    ck(p.st.batch_requested == 0, "and no writes are queued")
    reply = p.send(b"\xfe\x5a")
    ck(reply == b"\xa5", "the stream is still in sync", repr(reply))


def main():
    tmp = tempfile.mkdtemp(prefix="gbfw-host-")
    try:
        so = build_lib(tmp)
        p = Proto(so)
        for t in (test_fw_info, test_set_variable_acks,
                  test_size_selects_the_table, test_transfer_size_is_clamped,
                  test_modes_are_acked,
                  test_agb_mode_drops_the_rail_immediately,
                  test_five_volts_needs_dmg_mode,
                  test_dmg_bank_switch_is_a_plain_write,
                  test_dmg_read_advances_by_bytes,
                  test_clk_toggle_and_mbc_reset,
                  test_set_flash_cmd_carries_the_chip_sequence,
                  test_cart_write_flash_cmd_is_variable_length,
                  test_set_step_pins_and_unpins,
                  test_flash_program_payload,
                  test_a_program_block_split_across_packets,
                  test_an_oversized_payload_is_consumed_not_truncated,
                  test_ping_answers_the_challenge,
                  test_switch_state_answers_zero,
                  test_an_oversized_batch_is_refused_not_clipped,
                  test_the_three_byte_five_volt_injection,
                  test_a_random_save_restore_dispatches_nothing,
                  test_an_abandoned_payload_times_out,
                  test_a_slow_but_live_host_is_not_abandoned,
                  test_a_usb_link_reset_abandons_the_parser,
                  test_save_memory_reads_and_writes,
                  test_flash_save_write_is_not_a_plain_sram_write,
                  test_the_flash_write_configuration_is_not_discarded,
                  test_debug_is_acked,
                  test_bank_change_command_is_held,
                  test_the_boot_handshake_runs_only_on_the_agb_bus,
                  test_the_rtc_read_is_deferred_but_its_length_is_not,
                  test_the_3d_memory_page_terminator_is_swallowed,
                  test_the_intel_command_set_is_not_driven_as_amd,
                  test_an_undriveable_flash_method_is_refused,
                  test_eeprom_write_does_not_poison_the_next_flash_save,
                  test_stream_progress_cleared_on_block_open,
                  test_3d_page_terminator_survives_the_automatic_close,
                  test_dmg_rom_bank_mirrors_into_last_bank_accessed,
                  test_agb_cart_write_and_crc32,
                  test_var_state_round_trips,
                  test_var_state_never_restores_the_rail, test_unimplemented_vs_unknown,
                  test_resync_is_answered,
                  test_stream_is_position_independent, test_a_realistic_session,
                  test_capability_flags, test_bootloader_reset_sequence,
                  test_stray_confirm_is_harmless,
                  test_agb_read_signals_rather_than_replies,
                  test_agb_header_commands_do_not_crash_the_host,
                  test_set_pin_only_touches_power,
                  test_power_reports_hardware_not_intent,
                  test_get_variable_always_four_bytes):
            p.lib.fw_proto_init(ctypes.byref(p.st))
            t(p)
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    sys.stdout.write("test_proto: %d checks, %d failures\n"
                     % (CHECKS, len(FAILS)))
    for f in FAILS:
        sys.stdout.write("  failed: %s\n" % f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
