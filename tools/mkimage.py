#!/usr/bin/env python3
"""Wrap a linked firmware payload in the LFBG container FlashGBX installs.

0x200-byte boot-info record at flash 0x3E00, payload at 0x4000. File offset of
a flash address is `addr - 0x3E00`.

    off 0x000  u16  marker      0xFFFF
    off 0x002  4    tag         "LFBG"
    off 0x006  u16  app CRC16   over the whole payload
    off 0x008  u32  applen      payload length in bytes
    off 0x00C  u16  record CRC16 over bytes 0x00..0x0B
    off 0x00E  ..   0xFF fill to 0x200

Little-endian. CRC is MODBUS (init 0xFFFF, reflected poly 0xA001), matching the
bootloader and FlashGBX.

The record CRC at 0x0C covers the marker, so stamping a different marker after
the fact invalidates the record and the bootloader refuses the image.
"""

import argparse
import os
import struct
import sys

HDR_PAGE_LEN = 0x200
BOOTINFO_BASE = 0x3E00
APP_BASE = 0x4000
APP_MIN_LEN = 0x90
APP_MAX_LEN = 0x38800   # official bootloader cap
CODEFLASH_END = 0x3E800

OFF_MARKER = 0x00
OFF_TAG = 0x02
OFF_APPCRC = 0x06
OFF_APPLEN = 0x08
OFF_HDRCRC = 0x0C
HDR_CRC_COVERAGE = 0x0C

MARKER = 0xFFFF
TAG = b"LFBG"
FILL = 0xFF

SP_MASK = 0x2FFE0000
SP_VALUE = 0x20000000


def crc16(data):
    """MODBUS CRC16. Must match bl_crc16() and FlashGBX."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def build(payload):
    if len(payload) % 2:
        # Device programs in 16-bit units; an odd tail is padded on the way in
        # and the CRC would then disagree with flash.
        payload = payload + bytes([FILL])

    hdr = bytearray([FILL]) * HDR_PAGE_LEN
    struct.pack_into("<H", hdr, OFF_MARKER, MARKER)
    hdr[OFF_TAG:OFF_TAG + 4] = TAG
    struct.pack_into("<H", hdr, OFF_APPCRC, crc16(payload))
    struct.pack_into("<I", hdr, OFF_APPLEN, len(payload))
    struct.pack_into("<H", hdr, OFF_HDRCRC, crc16(hdr[:HDR_CRC_COVERAGE]))
    return bytes(hdr) + payload


def gates(image):
    """Device acceptance gates, reimplemented, plus local-only ones.

    ota_check_app tests the record CRC16 over 0x00..0x0B, applen <= 0x38800,
    and the payload CRC16; jump_to_app adds vector[0]'s SP mask
    (results/OFFICIAL-BOARD-TRIAGE.md 2.5-2.6). Marker, LFBG tag, minimum
    length, erased-fill tail, reset vector and vectors 1..35 are local policy,
    unenforced by retail hardware: retail BLXes a bad vector[1] and faults on
    handoff. A gate passing here does not mean the device checks it.

    Do not factor these into a helper shared with the CRC/header writer; a
    single implementation would agree with itself by construction.
    """
    out = []

    def ck(ok, what, detail=""):
        out.append((bool(ok), what, detail))

    ck(len(image) > HDR_PAGE_LEN, "image is larger than the boot-info page",
       "%d bytes" % len(image))
    if len(image) <= HDR_PAGE_LEN:
        return out

    hdr, payload = image[:HDR_PAGE_LEN], image[HDR_PAGE_LEN:]
    marker = struct.unpack_from("<H", hdr, OFF_MARKER)[0]
    applen = struct.unpack_from("<I", hdr, OFF_APPLEN)[0]
    appcrc = struct.unpack_from("<H", hdr, OFF_APPCRC)[0]
    hdrcrc = struct.unpack_from("<H", hdr, OFF_HDRCRC)[0]

    ck(marker in (0xFFFF, 0x5555), "marker is 0xFFFF or 0x5555",
       "0x%04X" % marker)
    ck(hdr[OFF_TAG:OFF_TAG + 4] == TAG, "tag is 'LFBG'",
       repr(bytes(hdr[OFF_TAG:OFF_TAG + 4])))
    ck(crc16(hdr[:HDR_CRC_COVERAGE]) == hdrcrc,
       "record CRC16 over 0x00..0x0B",
       "stored 0x%04X, computed 0x%04X" % (hdrcrc, crc16(hdr[:HDR_CRC_COVERAGE])))
    ck(APP_MIN_LEN <= applen <= APP_MAX_LEN, "applen is within the budget",
       "0x%X" % applen)
    ck(applen == len(payload), "applen matches the payload present",
       "field 0x%X, present 0x%X" % (applen, len(payload)))
    ck(APP_BASE + applen <= CODEFLASH_END, "image fits in CodeFlash")
    ck(crc16(payload) == appcrc, "payload CRC16",
       "stored 0x%04X, computed 0x%04X" % (appcrc, crc16(payload)))
    ck(all(b == FILL for b in hdr[0x0E:]),
       "rest of the boot-info page is erased fill")

    if len(payload) >= 8:
        sp, pc = struct.unpack_from("<II", payload, 0)
        ck((sp & SP_MASK) == SP_VALUE, "initial SP is SRAM-shaped",
           "0x%08X" % sp)
        ck(pc & 1, "reset vector has the Thumb bit set", "0x%08X" % pc)
        ck(APP_BASE <= (pc & ~1) < APP_BASE + applen,
           "reset vector lands inside the image", "0x%08X" % pc)

    if len(payload) >= APP_MIN_LEN:
        vecs = struct.unpack_from("<36I", payload, 0)
        bad = [i for i, v in enumerate(vecs[1:], 1)
               if not (v & 1) or not (APP_BASE <= (v & ~1) < APP_BASE + applen)]
        ck(not bad, "every vector 1..35 is Thumb and inside the image",
           "bad: %s" % bad[:6] if bad else "")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("payload", help="the linked .bin, based at 0x4000")
    ap.add_argument("-o", "--out", required=True, help="the fw.bin to write")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    with open(args.payload, "rb") as f:
        payload = f.read()
    if not payload:
        sys.exit("mkimage: %s is empty" % args.payload)

    image = build(payload)
    results = gates(image)
    failed = [r for r in results if not r[0]]

    if not args.quiet or failed:
        for ok, what, detail in results:
            print("  [%s] %s%s" % ("PASS" if ok else "FAIL", what,
                                   (": " + detail) if detail else ""))
    if failed:
        sys.exit("\nmkimage: %d gate(s) failed; not writing %s"
                 % (len(failed), args.out))

    d = os.path.dirname(args.out)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(image)

    if not args.quiet:
        print()
    print("  IMAGE   %s  %d bytes  (payload %d, flash 0x%04X..0x%X)"
          % (args.out, len(image), len(payload), BOOTINFO_BASE,
             APP_BASE + len(payload) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
