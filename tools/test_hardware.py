#!/usr/bin/env python3
"""Hardware regression tests that the offline suites cannot cover.

host/test_proto.py feeds the parser bytes directly and cannot see the USB layer,
where corruption inside a payload still leaves the framing valid.

Reads only. Nothing here writes to a cartridge.
"""
import argparse
import glob
import hashlib
import struct
import sys
import time

BAUD = 2000000
AGB_LOGO_SHA1 = bytes([0x17, 0xDA, 0xA0, 0xFE, 0xC0, 0x2F, 0xC3, 0x3C, 0x0F, 0x6A,
                       0xBB, 0x54, 0x9A, 0x8B, 0x80, 0xB6, 0x61, 0x3B, 0x48, 0xEE])
_pass = _fail = 0


def ck(cond, what, detail=""):
    global _pass, _fail
    if cond:
        _pass += 1
        print("  [ ok ] %s" % what)
    else:
        _fail += 1
        print("  [FAIL] %s%s" % (what, (": %s" % detail) if detail else ""))


def fnv(b):
    h = 0x811C9DC5
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cart", action="store_true", help="a cartridge is inserted")
    ap.add_argument("--port", help="serial port; required on Windows, where "
                                   "ports are COMn and no glob finds them")
    args = ap.parse_args()

    import serial
    port = args.port
    if port is None:
        ports = sorted(glob.glob("/dev/cu.usbserial*")
                       + glob.glob("/dev/cu.usbmodem*")
                       + glob.glob("/dev/cu.wchusbserial*")
                       + glob.glob("/dev/ttyUSB*"))
        if ports:
            port = ports[0]
        else:
            from serial.tools import list_ports
            cands = sorted(p.device for p in list_ports.comports())
            if not cands:
                sys.exit("no serial port found. Pass --port")
            port = cands[0]
    d = serial.Serial(port, BAUD, timeout=6)
    time.sleep(0.4)
    while d.read(65536):
        pass
    d.reset_input_buffer()

    def sv(size, key, val):
        d.reset_input_buffer()
        d.write(bytes([0xA6, size]) + struct.pack(">I", key) + struct.pack(">I", val))
        d.flush()
        return d.read(1)

    d.write(bytes([0xA1])); d.flush()
    n = d.read(1)
    if len(n) != 1 or n[0] != 8:
        sys.exit("device is not answering the protocol")
    info = d.read(8); ln = d.read(1)[0]; name = d.read(ln); d.read(2)
    print("device: %s%d, PCB %d, %r\n"
          % (chr(info[0]), int.from_bytes(info[1:3], "big"), info[3],
             name.decode("ascii", "replace")))

    print("bulk receive is not corrupted at packet boundaries")
    # EP2 has two 64-byte OUT windows (RB_UEP2_BUF_MOD; bl_usb_ep2_dbuf is 1 in
    # every reachable configuration). The receive path must close the window
    # before copying out of it and must service the SIE per fed byte; leaving
    # the window open across the copy lets a fast host land a packet on one not
    # yet read.
    for size in (64, 96, 128, 256, 512, 1024, 2048):
        worst = None
        for _ in range(4):
            sv(2, 0x00, size)
            payload = bytes(((i * 13) ^ 0xA5) & 0xFF for i in range(size))
            d.reset_input_buffer()
            d.write(bytes([0xE7]) + payload)
            d.flush()
            r = d.read(4)
            got = struct.unpack(">I", r)[0] if len(r) == 4 else None
            if got != fnv(payload):
                worst = got
            time.sleep(0.01)
            d.reset_input_buffer()
        ck(worst is None, "%d-byte payload survives 4 round trips" % size,
           "checksum %s" % (("0x%08X" % worst) if worst else "short reply"))

    print("\nCALC_CRC32 replies exactly four bytes")
    # The host does _read(4) and nothing else. A fifth byte is one stale byte
    # per verify sector, ~128 of them on a 16 MiB ROM write, and every reply
    # after it is off by one.
    sv(2, 0x00, 0x100)
    sv(4, 0x00, 0)
    d.reset_input_buffer()
    d.write(bytes([0xD5]) + struct.pack(">I", 0x100))
    d.flush()
    crc = d.read(4)
    time.sleep(0.15)
    extra = d.in_waiting
    ck(len(crc) == 4 and extra == 0,
       "four CRC bytes and nothing after them",
       "%d bytes, %d stray" % (len(crc), extra))
    d.reset_input_buffer(); d.write(bytes([0xFE, 0x5A])); d.flush()
    ck(d.read(1) == b"\xa5", "and the very next reply is the PING answer")

    print("\nthe parser stays in sync after an over-long payload")
    sv(2, 0x00, 0x1000)
    d.reset_input_buffer()
    d.write(bytes([0xD3]) + bytes(0x1000))
    d.flush()
    r = d.read(1)
    time.sleep(0.2)
    stray = d.in_waiting
    ck(r == b"\x02" and stray == 0,
       "a 4096-byte FLASH_PROGRAM is refused with exactly one byte",
       "reply %r, %d stray" % (r, stray))
    d.reset_input_buffer(); d.write(bytes([0xFE, 0x5A])); d.flush()
    ck(d.read(1) == b"\xa5", "and PING still answers")

    if args.cart:
        print("\ncartridge read")
        d.reset_input_buffer(); d.write(bytes([0xA2])); d.flush(); d.read(1)
        d.reset_input_buffer(); d.write(bytes([0xF2])); d.flush(); d.read(1)
        time.sleep(1.3)
        sv(4, 0x00, 0); sv(2, 0x00, 0x200)
        d.reset_input_buffer(); d.write(bytes([0xC1])); d.flush()
        hdr = b""
        while len(hdr) < 0x200:
            c = d.read(0x200 - len(hdr))
            if not c:
                break
            hdr += c
        ck(len(hdr) == 0x200, "the header reads back at full length")
        if len(set(hdr)) == 1:
            print("  [SKIP] every byte is 0x%02X. The slot is EMPTY or unpowered,"
                  % hdr[0])
            print("         not a read failure. Insert a cartridge and re-run.")
        else:
            ck(hashlib.sha1(hdr[0x04:0xA0]).digest() == AGB_LOGO_SHA1,
               "the Nintendo logo matches byte for byte",
               "first 8: %s" % hdr[0x04:0x0C].hex())
        d.reset_input_buffer(); d.write(bytes([0xF3])); d.flush(); d.read(1)

    d.close()
    print("\ntest_hardware: %d passed, %d failed" % (_pass, _fail))
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
