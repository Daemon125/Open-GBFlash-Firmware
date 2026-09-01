#!/usr/bin/env python3
"""Read a DMG/GBC cartridge with every read method and prove they agree.

    python3 tools/verify_dmg_methods.py --kib 256
    python3 tools/verify_dmg_methods.py --compare work/reference.gb

The FlashGBX GUI exposes only two of the three read methods, so a defect in the
third reaches nobody testing through the GUI.

  method 0  RD      drive the address, pulse /RD, sample
  method 1  A15     no /RD at all; A15 going high terminates the cycle
  method 2  SlowA15 the same with roughly four times the settle

Agreement is not a gate: methods sharing a wrong assumption agree perfectly, a
floating bus included. --compare against a reference taken with the VENDOR
firmware is the real gate.
"""

import argparse
import glob
import hashlib
import struct
import sys
import time

BAUD = 2000000
BANK_SIZE = 0x4000

METHODS = [(0, "RD"), (1, "A15"), (2, "SlowA15")]


class Dev:
    def __init__(self, port=None):
        import serial
        if port is None:
            ports = sorted(glob.glob("/dev/cu.usbserial*")
                           + glob.glob("/dev/cu.usbmodem*")
                           + glob.glob("/dev/cu.wchusbserial*")
                           + glob.glob("/dev/ttyUSB*"))
            if not ports:
                sys.exit("no device")
            port = ports[0]
        self.d = serial.Serial(port, BAUD, timeout=8)
        time.sleep(0.4)
        while self.d.read(65536):
            pass

    def cmd(self, b, n=1):
        self.d.reset_input_buffer()
        self.d.write(b if isinstance(b, (bytes, bytearray)) else bytes([b]))
        self.d.flush()
        return self.d.read(n) if n else b""

    def sv(self, size, key, val):
        return self.cmd(bytes([0xA6, size]) + struct.pack(">I", key)
                        + struct.pack(">I", val))

    def cart_write(self, addr, value):
        return self.cmd(bytes([0xB2]) + struct.pack(">I", addr) + bytes([value]))

    def read(self, addr, length):
        self.sv(4, 0x00, addr)
        # Only on a change: re-sending it every chunk is a round trip per
        # chunk, which is a third of the measured rate on this harness.
        if getattr(self, "_last_len", None) != length:
            self.sv(2, 0x00, length)
            self._last_len = length
        self.d.reset_input_buffer()
        self.d.write(bytes([0xB1]))
        self.d.flush()
        out = b""
        while len(out) < length:
            c = self.d.read(length - len(out))
            if not c:
                break
            out += c
        return out

    def dump(self, banks, chunk):
        out = bytearray()
        for bank in range(banks):
            if bank == 0:
                base, window = 0x0000, BANK_SIZE
            else:
                self.cart_write(0x2100, bank & 0xFF)
                if banks > 256:
                    self.cart_write(0x3000, (bank >> 8) & 1)
                base, window = 0x4000, BANK_SIZE
            off = 0
            while off < window:
                want = min(chunk, window - off)
                got = self.read(base + off, want)
                if len(got) != want:
                    return None
                out += got
                off += want
        return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port")
    ap.add_argument("--kib", type=int, default=0,
                    help="how much to read; default is the whole cartridge")
    ap.add_argument("--chunk", type=lambda x: int(x, 0), default=0x1000)
    ap.add_argument("--compare", help="known-good file, ideally taken with the "
                                      "vendor firmware")
    ap.add_argument("--banks", type=lambda x: int(x, 0), default=0,
                    help="override the header's bank count: a repro is "
                         "routinely bigger than the game burnt onto it")
    args = ap.parse_args()

    dev = Dev(args.port)
    if dev.cmd(bytes([0xA1])) != bytes([8]):
        sys.exit("device is not answering the protocol")
    info = dev.d.read(8)
    ln = dev.d.read(1)[0]
    name = dev.d.read(ln)
    dev.d.read(2)
    print("device: %s%d, PCB %d, %r"
          % (chr(info[0]), int.from_bytes(info[1:3], "big"), info[3],
             name.decode("ascii", "replace")))

    dev.cmd(0xA3)                       # DMG mode
    dev.cmd(0xF2)                       # cartridge power on
    time.sleep(1.3)

    hdr = dev.read(0x0000, 0x180)
    if len(hdr) < 0x180:
        dev.cmd(0xF3)
        sys.exit("short header read. Is a cartridge inserted?")
    title = hdr[0x134:0x143].split(b"\x00")[0].decode("ascii", "replace")
    banks = args.banks if args.banks else (2 << hdr[0x148])
    if args.kib:
        banks = max(1, (args.kib * 1024) // BANK_SIZE)
    print("  title %r, %d banks = %d KiB\n" % (title, banks, banks * BANK_SIZE // 1024))

    results = {}
    for m, label in METHODS:
        dev.sv(1, 0x0B, m)              # DMG_READ_METHOD
        t0 = time.time()
        got = dev.dump(banks, args.chunk)
        dt = time.time() - t0
        if got is None:
            print("  %-8s SHORT READ" % label)
            results[label] = None
            continue
        results[label] = got
        print("  %-8s %6.2f s   %7.1f KiB/s   md5 %s"
              % (label, dt, len(got) / 1024 / dt, hashlib.md5(got).hexdigest()))
    dev.cmd(0xF3)                       # cartridge power off

    print("\nagreement")
    fails = 0
    ref_label = "RD"
    ref = results.get(ref_label)
    if ref is None:
        print("  [fail] %s did not complete; nothing to compare against" % ref_label)
        return 1
    for _, label in METHODS:
        if label == ref_label:
            continue
        got = results.get(label)
        if got is None:
            print("  [fail] %s did not complete" % label)
            fails += 1
            continue
        if got == ref:
            print("  [ ok ] %s matches %s over %d bytes" % (label, ref_label, len(ref)))
        else:
            bad = [i for i in range(min(len(got), len(ref))) if got[i] != ref[i]]
            print("  [fail] %s differs from %s in %d bytes, first at 0x%X"
                  % (label, ref_label, len(bad), bad[0] if bad else -1))
            fails += 1

    if args.compare:
        want = open(args.compare, "rb").read()
        print("\nagainst %s" % args.compare)
        for _, label in METHODS:
            got = results.get(label)
            if got is None:
                continue
            n = min(len(got), len(want))
            if got[:n] == want[:n]:
                print("  [ ok ] %s matches the reference over %d bytes" % (label, n))
            else:
                bad = [i for i in range(n) if got[i] != want[i]]
                print("  [fail] %s differs from the reference in %d bytes, "
                      "first at 0x%X" % (label, len(bad), bad[0]))
                fails += 1
    else:
        print("\n  NOTE: no --compare given. Agreement alone cannot catch a "
              "shared wrong assumption; supply a reference taken with the "
              "vendor firmware to make this a real gate.")

    print()
    if fails:
        print("verify_dmg_methods: %d FAILED" % fails)
        return 1
    print("verify_dmg_methods: all methods agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
