#!/usr/bin/env python3
"""Reproduce the short-final-packet receive corruption, and prove when it is fixed.

No cartridge is touched: neither opcode used here drives a cartridge pin.

A host-to-device transfer ending in a short packet (a total that is not a
multiple of the bulk OUT wMaxPacketSize, 64 on the shipping CDC build, 32 on the
CH340 fallback) sometimes leaves one 32-bit word wrong in the receive buffer,
always the first word of the last full packet, at payload offset
(size - 1 - wMaxPacketSize) for a one-byte tail since the opcode shifts the
payload one byte into the stream.

[MEASURED] macOS, 2500 trips per size, 32-byte CH340 endpoint: payloads 511 and
479 (aligned) 0 bad; 512 and 480 (short tail) 0.56% and 0.40%. 5000 aligned
transfers also gave zero. The trigger is the short packet, not size, payload, or
timing (a 4 ms inter-transfer gap does not prevent it).

FlashGBX writes ROM in 2048-byte blocks (hw_GBFlash.py:68), each a 2049-byte
stream with a 1-byte tail, so every ROM write and save restore is exposed at the
measured 0.07% for that size. The block is the right length and the device
reports success, so only the host's verify pass catches it.
"""

import argparse
import glob
import struct
import sys
import time

BAUD = 2000000          # match FlashGBX (hw_GBFlash.py:311); 1 Mbaud caps at ~97 KiB/s
CMD_QUERY_FW_INFO = 0xA1
CMD_SET_VARIABLE = 0xA6
CMD_ECHO_PAYLOAD = 0xE7
CMD_DUMP_PAYLOAD = 0xE6

# The 479/480 pair is not a control on the 64-byte CDC build: 480 = 7x64 + 32,
# so (479, "aligned") is itself a short tail there. 512 and 2048 are whole
# multiples of both 64 and 32 and hold on either endpoint. Change 479/480 to
# 447/448 (448 = 7x64 = 14x32) before reading that row.
CASES = [
    (511, "aligned"), (512, "short tail"),
    (479, "aligned"), (480, "short tail"),
    (2047, "aligned"), (2048, "short tail"),   # 2048 is FlashGBX's write block
]

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


def fnv(b):
    h = 0x811C9DC5
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbserial*")
                          + glob.glob("/dev/cu.usbmodem*")
                   + glob.glob("/dev/cu.wchusbserial*")
                   + glob.glob("/dev/ttyUSB*"))
    if ports:
        return ports[0]
    try:
        from serial.tools import list_ports
        cands = [p.device for p in list_ports.comports()
                 if (p.vid, p.pid) == (0x1A86, 0x7523)]
        if cands:
            return sorted(cands)[0]
    except Exception:
        pass
    sys.exit("no GBFlash serial port found. Pass --port")


class Dev:
    def __init__(self, port):
        import serial
        self.d = serial.Serial(port, BAUD, timeout=6)
        time.sleep(0.4)
        while self.d.read(65536):
            pass

    def setvar16(self, key, val):
        self.d.reset_input_buffer()
        self.d.write(bytes([CMD_SET_VARIABLE, 2]) + struct.pack(">I", key)
                     + struct.pack(">I", val))
        self.d.flush()
        return self.d.read(1)

    def echo(self, payload):
        self.d.reset_input_buffer()
        self.d.write(bytes([CMD_ECHO_PAYLOAD]) + payload)
        self.d.flush()
        return self.d.read(4)

    def dump(self, size):
        """Returns the previous command's buffer. Carries no payload of its
        own, so it does not overwrite what it is reporting."""
        self.d.reset_input_buffer()
        self.d.write(bytes([CMD_DUMP_PAYLOAD]))
        self.d.flush()
        hdr = self.d.read(2)
        if len(hdr) != 2:
            return None, -1
        return self.d.read(size), struct.unpack(">H", hdr)[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port")
    ap.add_argument("--trips", type=int, default=2000)
    ap.add_argument("--locate", action="store_true",
                    help="on the first mismatch, dump and diff the buffer")
    args = ap.parse_args()

    dev = Dev(args.port or find_port())
    dev.d.reset_input_buffer()
    dev.d.write(bytes([CMD_QUERY_FW_INFO]))
    dev.d.flush()
    if dev.d.read(1) != bytes([8]):
        sys.exit("device is not answering the protocol")
    info = dev.d.read(8)
    ln = dev.d.read(1)[0]
    name = dev.d.read(ln)
    dev.d.read(2)
    print("device: %s%d, PCB %d, %r   platform %s\n"
          % (chr(info[0]), int.from_bytes(info[1:3], "big"), info[3],
             name.decode("ascii", "replace"), sys.platform))

    print("%d round trips per size. Aligned rows are the control." % args.trips)
    print("\n  %7s %8s %-11s %8s %8s" % ("payload", "stream", "shape", "bad",
                                         "rate"))
    print("  %7s %8s %-11s %8s %8s" % ("-" * 7, "-" * 8, "-" * 11, "-" * 8,
                                       "-" * 8))
    rates = {}
    located = None
    for size, shape in CASES:
        dev.setvar16(0x00, size)
        payload = bytes(((i * 13) ^ 0xA5) & 0xFF for i in range(size))
        want = fnv(payload)
        bad = 0
        for _ in range(args.trips):
            r = dev.echo(payload)
            if len(r) == 4 and struct.unpack(">I", r)[0] == want:
                continue
            bad += 1
            if args.locate and located is None:
                got, pay_got = dev.dump(size)
                if got and len(got) == size:
                    diff = [i for i in range(size) if got[i] != payload[i]]
                    located = (size, diff, payload, got, pay_got)
        rates[size] = bad / float(args.trips)
        print("  %7d %8d %-11s %8d %7.2f%%"
              % (size, size + 1, shape, bad, 100.0 * bad / args.trips))

    print("\nverdict")
    aligned = [s for s, shape in CASES if shape == "aligned"]
    short = [s for s, shape in CASES if shape == "short tail"]
    ck(all(rates[s] == 0.0 for s in aligned),
       "transfers that are a whole number of 32-byte packets are clean",
       "rates: %s" % {s: rates[s] for s in aligned})
    worst = max(rates[s] for s in short)
    if ck(worst == 0.0,
          "transfers ending in a short packet are clean too",
          "worst %.2f%%. The bug is still present" % (100.0 * worst)):
        print("\n  Both clean. Note the rate at 2048 is only ~0.07% when the bug")
        print("  IS present, so re-run with --trips 5000 before trusting this.")

    if located:
        size, diff, payload, got, pay_got = located
        print("\nwhat the damage looked like (payload size %d)" % size)
        print("  device accumulated pay_got=%d, host sent %d" % (pay_got, size))
        print("  %d byte(s) wrong, first at offset %d" % (len(diff), diff[0]))
        print("  expected offset %d = size-33 = the first word of the last full"
              % (size - 33))
        print("  packet, since the opcode shifts the payload one byte into the"
              " stream")
        i = diff[0]
        print("    sent  %s" % payload[max(0, i - 4):i + 8].hex(" "))
        print("    got   %s" % got[max(0, i - 4):i + 8].hex(" "))

    dev.d.close()
    print("\ntest_short_packet: %d passed, %d failed" % (_pass, _fail))
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
