#!/usr/bin/env python3
"""ReadROM must not re-send firmware variables the device already holds.

_BackupROM reads DMG one 0x4000 bank per call (LK_Device.py:3003), so upstream
sends TRANSFER_SIZE, ADDRESS and DMG_ACCESS_MODE every 16 KiB. Only ADDRESS
changes. The other two are not re-sent; see results/dmg-variable-resend.md.

Anything else that writes those variables must make the next call send them
again, or a dump can run with a transfer size the device does not have.
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
for extra in (os.path.abspath(os.path.join(ROOT, "..", "..", "FlashGBX-stock")),
              os.path.abspath(os.path.join(ROOT, "..", "..", "FlashGBX"))):
    if os.path.isdir(os.path.join(extra, "FlashGBX")) and extra not in sys.path:
        sys.path.append(extra)

SET_VARIABLE = 0xA6


class CountingPort:
    """Answers every command with one ACK and records the opcodes written."""

    def __init__(self, reply_len):
        self.timeout = 1.0
        self.reply_len = reply_len
        self.pending = bytearray()
        self.ops = []

    def write(self, data):
        data = bytes(data)
        self.ops.append(data[0])
        # a read opcode returns a payload, everything else one ACK
        self.pending += (b"\x5A" * self.reply_len) if data[0] in (0xB1, 0xC1) else b"\x01"
        return len(data)

    def flush(self): pass

    def read(self, n=1):
        out = bytes(self.pending[:n]); del self.pending[:len(out)]
        return out

    def reset_input_buffer(self): self.pending = bytearray()
    def reset_output_buffer(self): pass

    @property
    def in_waiting(self): return len(self.pending)


def make(cls, mode, reply_len, open_fw=True):
    dev = cls.__new__(cls)
    dev.DEVICE = CountingPort(reply_len)
    dev.CANCEL_ARGS = {}; dev.ERROR = False; dev.CANCEL = False
    dev.OPEN_FW = open_fw; dev.MODE = mode
    dev.FW = {"fw_ver": 15, "pcb_name": "Open-GBFlash"}
    dev.FW_VAR = {}
    dev.WRITE_DELAY = False; dev.READ_ERRORS = 0
    dev.MAX_BUFFER_READ = 0x2000
    dev.NO_PROG_UPDATE = True
    dev.INFO = {"action": None}
    return dev


def setvars(dev):
    return dev.DEVICE.ops.count(SET_VARIABLE)


def main():
    print("test_rom_var_resend: only ADDRESS may be re-sent per call")
    try:
        from FlashGBX.LK_Device import LK_Device       # noqa: F401
        import importlib.util
    except ImportError as e:
        print("  skipped, FlashGBX is not importable here (%s)" % e)
        return 0
    spec = importlib.util.spec_from_file_location(
        "FlashGBX.hw_GBFlash_repo2", os.path.join(ROOT, "hw_GBFlash.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    G = mod.GbxDevice
    fails = 0

    # Four DMG banks, as _BackupROM drives them: 0x4000 per call.
    dev = make(G, "DMG", 0x2000)
    for bank in range(4):
        dev.ReadROM(0x4000, 0x4000, max_length=0x2000)
    n = setvars(dev)
    print("  4 DMG bank reads: %d SET_VARIABLE (upstream sends 12)" % n)
    if n != 6:   # TRANSFER_SIZE + DMG_ACCESS_MODE once, ADDRESS x4
        print("  [FAIL] expected 6"); fails += 1

    # Anything else touching those variables must force a re-send.
    dev = make(G, "DMG", 0x2000)
    dev.ReadROM(0x4000, 0x4000, max_length=0x2000)
    before = setvars(dev)
    dev._set_fw_variable("DMG_ACCESS_MODE", 2)      # e.g. a save read
    dev.ReadROM(0x4000, 0x4000, max_length=0x2000)
    after = setvars(dev) - before - 1               # minus the intruding write
    print("  after an outside write: %d SET_VARIABLE on the next call" % after)
    if after != 3:
        print("  [FAIL] expected 3, the full init"); fails += 1

    # A different transfer size must be sent.
    dev = make(G, "DMG", 0x2000)
    dev.ReadROM(0x4000, 0x4000, max_length=0x2000)
    before = setvars(dev)
    dev.ReadROM(0x4000, 0x2000, max_length=0x1000)
    print("  after a size change: %d SET_VARIABLE" % (setvars(dev) - before))
    if (setvars(dev) - before) != 2:                # TRANSFER_SIZE + ADDRESS
        print("  [FAIL] expected 2"); fails += 1

    # Stock firmware must see upstream's behaviour: OPEN_FW is decided from the
    # reported pcb_name, and a GBFlash board on stock has the same VID/PID.
    dev = make(G, "DMG", 0x2000, open_fw=False)
    for bank in range(4):
        dev.ReadROM(0x4000, 0x4000, max_length=0x2000)
    n = setvars(dev)
    print("  stock firmware, 4 bank reads: %d SET_VARIABLE (upstream sends 12)" % n)
    if n != 12:
        print("  [FAIL] the skip must not apply when OPEN_FW is False"); fails += 1

    # SetAGBReadMethod must call through on stock.
    seen = []
    dev = make(G, "AGB", 0x2000, open_fw=False)
    dev.AGB_READ_METHOD = 2
    dev.ACTIONS = {"ROM_READ": 1}
    dev.INFO = {"action": 1}
    dev._set_fw_variable = lambda k, v: seen.append((k, v))
    dev.SetAGBReadMethod(0)
    print("  stock firmware, SetAGBReadMethod(0): %s" % ("applied" if seen else "SUPPRESSED"))
    if not seen:
        print("  [FAIL] the downgrade must still apply when OPEN_FW is False"); fails += 1

    print("test_rom_var_resend: %s" % ("FAILED" if fails else "passed"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
