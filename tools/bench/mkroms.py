#!/usr/bin/env python3
"""Build the pair of test ROMs a write benchmark needs.

FlashGBX skips 2048-byte blocks that are all 0xFF, and skips blocks that
already match what is on the cartridge. So timing a write against a real ROM
times only the blocks that ROM happens to fill, and comparing two firmwares on
two different ROMs measures the ROMs rather than the firmware. A and B here are
seeded pseudorandom with no blank block and no block in common, asserted below.

The header checksum is computed so FlashGBX does not stop at an interactive
"fix the header checksum?" prompt. The boot logo is not: it is Nintendo's, it
is not reproduced here, and an absent bootlogo_*.bin in the config folder turns
that prompt into a warning.
"""

import os
import random
import sys

BLOCK = 2048


def body(seed, size):
    rng = random.Random(seed)
    try:
        return bytearray(rng.randbytes(size))
    except AttributeError:
        return bytearray(rng.getrandbits(8) for _ in range(size))


def agb(seed, size, title):
    d = body(seed, size)
    d[0x00:0x04] = b"\x2E\x00\x00\xEA"            # branch past the header
    d[0xA0:0xB0] = title.ljust(12)[:12].encode() + b"BENCHGBF"[:4]
    d[0xB0:0xBD] = bytes(13)
    d[0xBD] = (-(0x19 + sum(d[0xA0:0xBD]))) & 0xFF
    d[0xBE:0xC0] = bytes(2)
    return bytes(d)


def dmg(seed, size, title):
    d = body(seed, size)
    d[0x100:0x104] = b"\x00\xC3\x50\x01"          # nop; jp 0x0150
    d[0x134:0x144] = title.ljust(16)[:16].encode()
    d[0x144:0x147] = bytes(3)
    d[0x147] = 0x13                               # MBC3 + RAM + battery
    d[0x148] = {0x8000 << i: i for i in range(9)}.get(size, 0x06)
    d[0x149] = 0x03                               # 32 KiB SRAM
    d[0x14A:0x14D] = bytes(3)
    c = 0
    for b in d[0x134:0x14D]:
        c = (c - b - 1) & 0xFF
    d[0x14D] = c
    return bytes(d)


def check(name, data):
    blank = b"\xFF" * BLOCK
    n = sum(1 for i in range(0, len(data), BLOCK)
            if data[i:i + BLOCK] == blank)
    if n:
        sys.exit("%s has %d blank blocks; FlashGBX would skip them" % (name, n))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(
        os.path.abspath(__file__))
    os.makedirs(out, exist_ok=True)
    made = []
    for kind, size, ext in (("agb", 16 * 1024 * 1024, "gba"),
                            ("dmg", 2 * 1024 * 1024, "gb")):
        f = agb if kind == "agb" else dmg
        for tag, seed in (("a", 1), ("b", 2)):
            name = "%s_%s.%s" % (kind, tag, ext)
            data = f(seed, size, "BENCH" + tag.upper())
            check(name, data)
            with open(os.path.join(out, name), "wb") as fh:
                fh.write(data)
            made.append((kind, tag, name, data))
            print("  %-14s %9d bytes" % (name, len(data)))
    for kind in ("agb", "dmg"):
        a = next(d for k, t, _, d in made if k == kind and t == "a")
        b = next(d for k, t, _, d in made if k == kind and t == "b")
        shared = sum(1 for i in range(0, len(a), BLOCK)
                     if a[i:i + BLOCK] == b[i:i + BLOCK])
        if shared:
            sys.exit("%s A and B share %d blocks" % (kind, shared))
    print("  no blank blocks, no shared blocks between A and B")


if __name__ == "__main__":
    main()
