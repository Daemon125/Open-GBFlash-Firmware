#!/usr/bin/env python3
"""A SET_VARIABLE whose ACK is late must not leave a byte on the wire.

Upstream's _try_write flushes the input buffer, writes 0x00 and takes the next
byte as its answer. An ACK still in flight lands after that flush, is taken for
the 0x00's answer, and the 0x00's own ACK is then read as the re-sent command's.
The command's real ACK is left over and is read as the first byte of the next
bulk transfer, which shifts the rest of a ROM dump by one byte; see
results/rom-dump-byte-shift.md.
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
for extra in (os.path.abspath(os.path.join(ROOT, "..", "..", "FlashGBX-stock")),
              os.path.abspath(os.path.join(ROOT, "..", "..", "FlashGBX"))):
    if os.path.isdir(os.path.join(extra, "FlashGBX")) and extra not in sys.path:
        sys.path.append(extra)


class FakePort:
    """Answers every command with one ACK. The first one is late.

    It lands in the one window nothing flushes: after the resync loop's
    reset_input_buffer() and before the read that follows its 0x00. _read's own
    error path already drains to quiet, so an ACK late by any lesser amount is
    discarded harmlessly and there is no bug to see.
    """

    def __init__(self):
        self.timeout = 1.0
        self.pending = bytearray()
        self.inflight = []
        self.writes = []
        self._late_budget = 1

    def write(self, data):
        data = bytes(data)
        self.writes.append(data)
        if data == b"\x00" and self.inflight:
            self.pending += self.inflight.pop(0)      # ahead of this write's own ACK
        if self._late_budget > 0:
            self._late_budget -= 1
            self.inflight.append(b"\x01")
        else:
            self.pending += b"\x01"
        return len(data)

    def flush(self):
        pass

    def read(self, n=1):
        out = bytes(self.pending[:n])
        del self.pending[:len(out)]
        return out

    def reset_input_buffer(self):
        self.pending = bytearray()

    def reset_output_buffer(self):
        pass

    @property
    def in_waiting(self):
        return len(self.pending)


def run(cls, label, open_fw=True):
    dev = cls.__new__(cls)
    dev.DEVICE = FakePort()
    dev.CANCEL_ARGS = {}
    dev.ERROR = False
    dev.CANCEL = False
    dev.OPEN_FW = open_fw
    dev.FW = {"fw_ver": 15, "pcb_name": "Open-GBFlash"}
    dev.WRITE_DELAY = False
    dev.READ_ERRORS = 0
    dev._try_write(bytearray([0xA6, 4, 0, 0, 0, 0, 0, 0, 0, 0]))
    left = dev.DEVICE.in_waiting + len(dev.DEVICE.inflight)
    print("  %-22s bytes left on the wire: %d" % (label, left))
    return left


def main():
    print("test_late_ack: a late ACK must not survive the resync")
    try:
        import importlib.util
        from FlashGBX.LK_Device import LK_Device
    except ImportError as e:
        print("  skipped, FlashGBX is not importable here (%s)" % e)
        return 0

    # The tracked file, not whatever is installed. Loaded inside the FlashGBX
    # package so its relative imports resolve.
    spec = importlib.util.spec_from_file_location(
        "FlashGBX.hw_GBFlash_repo", os.path.join(ROOT, "hw_GBFlash.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    GbxDevice = mod.GbxDevice
    print("  testing %s" % os.path.join(ROOT, "hw_GBFlash.py"))

    fails = 0
    if not hasattr(GbxDevice, "_try_write") or GbxDevice._try_write is LK_Device._try_write:
        print("  [FAIL] GbxDevice does not override _try_write")
        return 1
    if run(GbxDevice, "override") != 0:
        print("  [FAIL] the override left a byte on the wire")
        fails += 1
    # OPEN_FW False delegates to LK_Device._try_write, so this is upstream's
    # behaviour on a concrete instance.
    if run(GbxDevice, "upstream fallback", open_fw=False) == 0:
        print("  [FAIL] the fake port no longer reproduces the race")
        fails += 1
    print("test_late_ack: %s" % ("FAILED" if fails else "passed"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
