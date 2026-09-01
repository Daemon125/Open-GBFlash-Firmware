#!/usr/bin/env python3
"""Dump an AGB cartridge to a file, or compare it against an earlier dump.

    python3 tools/dump_agb.py --mib 4 --out ref.bin
    python3 tools/dump_agb.py --mib 4 --compare ref.bin

verify_agb_methods.py cannot gate a bus-timing change: all three methods end in
the same strobe loop and shift together. Gate against a different BUILD instead.
Dump with the known-good firmware, flash the candidate, dump again on the same
cartridge in the same slot, compare byte for byte.
"""

import argparse
import hashlib
import os
import sys
import time

# os.path, not a split on "/": this file also ships in the Windows test folder,
# where a hardcoded separator breaks the import below.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_agb_methods import Dev, METHODS      # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port")
    ap.add_argument("--mib", type=float, default=4.0)
    ap.add_argument("--method", type=int, default=2, choices=(0, 1, 2))
    ap.add_argument("--out", help="write the dump here")
    ap.add_argument("--compare", help="compare against this file instead")
    ap.add_argument("--expect-md5",
                    help="compare against a known md5 rather than a file: "
                         "the same check across machines without moving a "
                         "cartridge dump between them")
    ap.add_argument("--passes", type=int, default=1,
                    help="dump this many times; marginal timing is intermittent")
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

    dev.cmd(0xA2)
    dev.cmd(0xF2)
    time.sleep(1.3)

    hdr = dev.read(0, 0xC0)
    if len(hdr) != 0xC0:
        dev.cmd(0xF3)
        sys.exit("short header read. Is a cartridge inserted?")
    logo_ok = hdr[4:12].hex() == "24ffae51699aa221"
    print("  %r, logo %s" % (hdr[0xA0:0xAC].split(b"\x00")[0],
                             "OK" if logo_ok else "BAD"))

    ref = None
    if args.expect_md5:
        print("  expecting md5 %s" % args.expect_md5)
    if args.compare:
        ref = open(args.compare, "rb").read()
        print("  reference %s, %d bytes, md5 %s"
              % (args.compare, len(ref), hashlib.md5(ref).hexdigest()))
        if len(ref) != n:
            dev.cmd(0xF3)
            sys.exit("reference is %d bytes but --mib asks for %d" % (len(ref), n))

    print("  reading %.2f MiB via %s, %d pass(es)\n"
          % (args.mib, METHODS[args.method], args.passes))

    bad = 0
    for p in range(args.passes):
        data, dt = dev.dump(n, args.method)
        if data is None:
            dev.cmd(0xF3)
            sys.exit("  pass %d: the read stopped short" % (p + 1))
        md5 = hashlib.md5(data).hexdigest()
        rate = (len(data) / dt / 1024.0) if dt else 0.0
        line = "  pass %d  %6.2f s  %7.1f KiB/s  md5 %s" % (p + 1, dt, rate, md5)

        if args.expect_md5:
            if md5 == args.expect_md5.lower():
                print(line + "   [ ok ] matches the expected md5")
            else:
                bad += 1
                print(line + "   [FAIL] expected %s" % args.expect_md5)
        elif ref is not None:
            if data == ref:
                print(line + "   [ ok ] identical to the reference")
            else:
                bad += 1
                diff = [i for i in range(n) if data[i] != ref[i]]
                # Halfword shift, the latch failure, reads as near-total
                # corruption in a plain diff.
                shifted = (data[2:] == ref[:-2])
                print(line + "   [FAIL] %d of %d bytes differ" % (len(diff), n))
                print("         first at 0x%X: got %02X%02X want %02X%02X"
                      % (diff[0], data[diff[0]], data[diff[0] + 1],
                         ref[diff[0]], ref[diff[0] + 1]))
                if shifted:
                    print("         the whole dump is SHIFTED FORWARD BY ONE"
                          " HALFWORD. The latch is coming up early")
        else:
            print(line)
        if args.out and p == 0:
            open(args.out, "wb").write(data)
            print("         written to %s" % args.out)

    dev.cmd(0xF3)
    if ref is not None or args.expect_md5:
        print("\n%s" % ("dump_agb: %d of %d pass(es) differ from the reference"
                        % (bad, args.passes) if bad else
                        "dump_agb: every pass is byte-identical to the reference"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
