#!/usr/bin/env python3
"""Cross-check the three AGB read methods against each other on a real cartridge.

    python3 tools/verify_agb_methods.py --mib 2

The AGB latch failure is silent: too short a settle after /CS and the
cartridge's internal counter comes up one halfword ahead, so every read shifts
forward by two bytes and every pass agrees on the same plausible wrong dump. An
empty slot cannot show it, since shifted open bus is still open bus.

The methods differ only in halfwords per address latch (Single 1, MemCpy 2,
Stream 64), so they exercise the latch at three very different rates and
throughput alone cannot tell a win from a corruption.
"""

import argparse
import glob
import struct
import sys
import time

BAUD = 2000000
METHODS = {0: "Single", 1: "MemCpy", 2: "Stream"}


class Dev:
    def __init__(self, port=None):
        import serial
        if port is None:
            port = sorted(glob.glob("/dev/cu.usbserial*")
                          + glob.glob("/dev/cu.usbmodem*")
                          + glob.glob("/dev/cu.wchusbserial*")
                          + glob.glob("/dev/ttyUSB*"))[0]
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

    def read(self, hwaddr, length):
        self.sv(4, 0x00, hwaddr)
        # TRANSFER_SIZE only moves on the last, short chunk. Re-sending it every
        # chunk cost a whole round trip per chunk: with the old 0x1000 chunk that
        # was 58% off the Single rate, which made every method comparison taken
        # through this harness a measurement of the harness.
        if getattr(self, "_last_len", None) != length:
            self.sv(2, 0x00, length)
            self._last_len = length
        self.d.reset_input_buffer()
        self.d.write(bytes([0xC1]))
        self.d.flush()
        out = b""
        while len(out) < length:
            c = self.d.read(length - len(out))
            if not c:
                break
            out += c
        return out

    def _read_seq(self, length):
        """One 0xC1 with ADDRESS and TRANSFER_SIZE already set."""
        self.d.reset_input_buffer()
        self.d.write(bytes([0xC1]))
        self.d.flush()
        out = b""
        while len(out) < length:
            c = self.d.read(length - len(out))
            if not c:
                break
            out += c
        return out

    def dump(self, nbytes, method, chunk=0x4000):
        # ADDRESS advances by itself across an AGB ROM read, so it is set once
        # for the whole dump, not once per chunk. Setting it every chunk is a
        # round trip per chunk and cost this harness a third of its rate.
        self.sv(1, 0x0C, method)        # VAR8_AGB_READ_METHOD
        self.sv(4, 0x00, 0)             # VAR32_ADDRESS, halfwords
        self._last_len = None
        out = bytearray()
        t0 = time.time()
        while len(out) < nbytes:
            want = min(chunk, nbytes - len(out))
            if self._last_len != want:
                self.sv(2, 0x00, want)
                self._last_len = want
            got = self._read_seq(want)
            if len(got) != want:
                return None, 0.0
            out += got
        return bytes(out), time.time() - t0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port")
    ap.add_argument("--mib", type=float, default=2.0)
    args = ap.parse_args()

    n = int(args.mib * 1024 * 1024)
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

    dev.cmd(0xA2)                       # AGB
    dev.cmd(0xF2)                       # power on
    time.sleep(1.3)

    hdr = dev.read(0, 0xC0)
    if len(hdr) != 0xC0:
        dev.cmd(0xF3)
        sys.exit("short header read. Is a cartridge inserted?")
    logo_ok = hdr[4:12].hex() == "24ffae51699aa221"
    print("  %r, logo %s\n" % (hdr[0xA0:0xAC].split(b"\x00")[0],
                               "OK" if logo_ok else "BAD"))

    print("  reading %.2f MiB three times\n" % args.mib)
    res = {}
    for m in (2, 1, 0):                 # Stream is the reference: it latches least
        data, dt = dev.dump(n, m)
        if data is None:
            dev.cmd(0xF3)
            sys.exit("short read in %s" % METHODS[m])
        res[m] = data
        print("  %-7s %6.2f s  %7.1f KiB/s" % (METHODS[m], dt, n / dt / 1024))

    dev.cmd(0xF3)
    dev.d.close()

    print("\nagreement")
    ref = res[2]
    bad = 0
    for m in (1, 0):
        diffs = [i for i in range(n) if res[m][i] != ref[i]]
        if not diffs:
            print("  [ ok ] %s matches Stream over %d bytes" % (METHODS[m], n))
            continue
        bad += 1
        print("  [FAIL] %s differs from Stream in %d of %d bytes"
              % (METHODS[m], len(diffs), n))
        i = diffs[0]
        print("         first at 0x%X" % i)
        print("           stream %s" % ref[i:i + 12].hex(" "))
        print("           %-6s %s" % (METHODS[m].lower(), res[m][i:i + 12].hex(" ")))
        if res[m][i:i + 32] == ref[i + 2:i + 34]:
            print("         SHIFTED BY ONE HALFWORD: the latch settle is too "
                  "short for this method's cadence")
    print("\nverify_agb_methods: %s" % ("FAIL" if bad else "all methods agree"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
