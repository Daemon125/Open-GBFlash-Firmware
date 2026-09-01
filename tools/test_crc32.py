#!/usr/bin/env python3
"""Check the device's CALC_CRC32 against the host's, over a real cartridge.

FlashGBX verifies a ROM write with CompareCRC32 (LK_Device.py), which asks the
device for the checksum instead of reading the data back, so a wrong CRC32
reports a verify result that never happened.

Reads only. Nothing is written.
"""

import argparse
import glob
import struct
import sys
import time
import zlib

BAUD = 2000000
_pass = _fail = 0


def ck(cond, what, detail=""):
    global _pass, _fail
    if cond:
        _pass += 1
        print("  [ ok ] %s" % what)
    else:
        _fail += 1
        print("  [FAIL] %s  [%s]" % (what, detail))
    return cond


class Dev:
    def __init__(self, port=None):
        import serial
        if port is None:
            port = sorted(glob.glob("/dev/cu.usbserial*")
                          + glob.glob("/dev/cu.usbmodem*")
                          + glob.glob("/dev/cu.wchusbserial*")
                          + glob.glob("/dev/ttyUSB*"))[0]
        self.d = serial.Serial(port, BAUD, timeout=30)
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

    def read(self, addr, length, agb):
        self.sv(4, 0x00, addr)
        self.sv(2, 0x00, length)
        self.d.reset_input_buffer()
        self.d.write(bytes([0xC1 if agb else 0xB1]))
        self.d.flush()
        out = b""
        while len(out) < length:
            c = self.d.read(length - len(out))
            if not c:
                break
            out += c
        return out

    def crc(self, length, agb):
        self.sv(4, 0x00, 0)
        self.d.reset_input_buffer()
        self.d.write(bytes([0xD5]) + struct.pack(">I", length))
        self.d.flush()
        r = self.d.read(4)
        return struct.unpack(">I", r)[0] if len(r) == 4 else None, len(r)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port")
    ap.add_argument("--mode", choices=["agb", "dmg"], default="agb")
    ap.add_argument("--max", type=lambda x: int(x, 0), default=0x400000)
    args = ap.parse_args()
    agb = args.mode == "agb"

    dev = Dev(args.port)
    if dev.cmd(bytes([0xA1])) != bytes([8]):
        sys.exit("device is not answering the protocol")
    info = dev.d.read(8)
    ln = dev.d.read(1)[0]
    name = dev.d.read(ln)
    dev.d.read(2)
    print("device: %s%d, PCB %d, %r  mode %s"
          % (chr(info[0]), int.from_bytes(info[1:3], "big"), info[3],
             name.decode("ascii", "replace"), args.mode.upper()))

    dev.cmd(0xA2 if agb else 0xA3)
    dev.cmd(0xF2)
    time.sleep(1.3)
    if agb:
        dev.sv(1, 0x0C, 2)              # Stream

    print("\n  reading %d KiB once, then checksumming it several ways\n"
          % (args.max // 1024))
    data = b""
    while len(data) < args.max:
        want = min(0x1000, args.max - len(data))
        got = dev.read((len(data) >> 1) if agb else len(data), want, agb)
        if len(got) != want:
            dev.cmd(0xF3)
            sys.exit("short read at 0x%X. Is a cartridge inserted?" % len(data))
        data += got

    lengths = [n for n in (0x1000, 0x8000, 0x40000, 0x100000, 0x400000)
               if n <= args.max]
    print("  %10s  %10s  %10s" % ("length", "device", "host"))
    for n in lengths:
        dev_crc, got = dev.crc(n, agb)
        host = zlib.crc32(data[:n]) & 0xFFFFFFFF
        if dev_crc is None:
            ck(False, "CALC_CRC32 over %d bytes replies four bytes" % n,
               "got %d" % got)
            continue
        ck(dev_crc == host,
           "CALC_CRC32 over %d bytes matches the host" % n,
           "device 0x%08X, host 0x%08X" % (dev_crc, host))

    # The reply must be exactly four bytes. A fifth is read as the next
    # command's reply and desynchronises everything after it.
    dev.sv(4, 0x00, 0)
    dev.d.reset_input_buffer()
    dev.d.write(bytes([0xD5]) + struct.pack(">I", 0x1000))
    dev.d.flush()
    time.sleep(0.4)
    extra = dev.d.in_waiting
    dev.d.read(extra)
    ck(extra == 4, "and replies with exactly four bytes, nothing trailing",
       "%d bytes waiting" % extra)

    dev.cmd(0xF3)
    dev.d.close()
    print("\ntest_crc32: %d passed, %d failed" % (_pass, _fail))
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
