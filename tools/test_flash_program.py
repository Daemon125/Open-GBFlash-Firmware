#!/usr/bin/env python3
"""Exercise FLASH_METHOD 1 and 2 (single-write and buffered) on real hardware.

Writes only to a region first confirmed erased and erases that sector again
afterwards. The blank check is what keeps this from eating cartridge data.
"""

import argparse
import glob
import struct
import sys
import time

BAUD = 2000000          # match FlashGBX (hw_GBFlash.py:311)

# [MEASURED] sector-granular scan of this part: 0x720000 to 0x1EFFFFF reads
# back erased, the last megabyte does not. 0x1F00000 holds real data.
TEST_BYTE_ADDR = 0x1E00000
SECTOR_BYTES = 0x20000          # MSP55LV128 family's uniform sector
LEN = 512

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


def find_port():
    """Windows ports are COMn, which no glob matches, so the pyserial
    enumeration fallback is the only path there."""
    ports = sorted(glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.usbmodem*")
                   + glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/ttyUSB*"))
    if ports:
        return ports[0]
    from serial.tools import list_ports
    cands = [p.device for p in list_ports.comports()]
    if not cands:
        sys.exit("no serial port found. Pass --port")
    return sorted(cands)[0]


class Dev:
    def __init__(self, port=None):
        import serial
        if port is None:
            port = find_port()
        self.d = serial.Serial(port, BAUD, timeout=6)
        time.sleep(0.4)
        while self.d.read(65536):
            pass

    def cmd(self, b, nread=1):
        self.d.reset_input_buffer()
        self.d.write(b if isinstance(b, (bytes, bytearray)) else bytes([b]))
        self.d.flush()
        return self.d.read(nread) if nread else b""

    def sv(self, size, key, val):
        """size is BYTES on the wire (1/2/4), not bits: _set_fw_variable maps
        the table's 8/16/32 down before sending (LK_Device.py:627-629)."""
        return self.cmd(bytes([0xA6, size]) + struct.pack(">I", key)
                        + struct.pack(">I", val))

    def gv(self, size, key):
        # _get_fw_variable does _read(4) and unpacks ">I" whatever the
        # variable's declared width. Reading `size` bytes desyncs the stream.
        r = self.cmd(bytes([0xAD, size]) + struct.pack(">I", key), 4)
        if len(r) != 4:
            return None
        v = struct.unpack(">I", r)[0]
        return v & ((1 << (size * 8)) - 1)

    def flash_cmds(self, cmd_set, method, pairs):
        buf = bytes([0xA7, cmd_set, method, 0])
        for i in range(6):
            a, v = pairs[i] if i < len(pairs) else (0, 0)
            buf += struct.pack(">I", a) + struct.pack(">H", v)
        return self.cmd(buf)

    def read_rom(self, hwaddr, length):
        self.sv(4, 0x00, hwaddr)
        self.sv(2, 0x00, length)
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

    def bus_write(self, hwaddr, value):
        """CART_WRITE_FLASH_CMD, flashcart=1, one pair."""
        return self.cmd(bytes([0xD4, 1, 1]) + struct.pack(">I", hwaddr)
                        + struct.pack(">H", value))


AMD_SINGLE = [(0xAAA >> 1, 0xAA), (0x555 >> 1, 0x55), (0xAAA >> 1, 0xA0), (0, 0)]
# SA/BS/PA/PD go out as zero for the device to substitute.
AMD_BUFFERED = [(0xAAA >> 1, 0xAA), (0x555 >> 1, 0x55), (0, 0x25),
                (0, 0), (0, 0), (0, 0x29)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port")
    args = ap.parse_args()
    dev = Dev(args.port)
    info = dev.cmd(bytes([0xA1]), 1)
    if info != bytes([8]):
        sys.exit("device is not answering the protocol")
    dev.d.read(8 + 1 + 32)
    dev.d.reset_input_buffer()

    dev.cmd(0xA2)                       # AGB mode
    dev.cmd(0xF2)                       # cartridge power on
    time.sleep(1.3)                     # AGB power settle

    hw = TEST_BYTE_ADDR >> 1
    sector_base = TEST_BYTE_ADDR & ~(SECTOR_BYTES - 1)
    print("checking the whole target sector is erased")
    # The restore step erases the whole 128 KiB sector, so the blank check has
    # to cover all of it, not just the 512 bytes being written.
    seen = set()
    off = 0
    while off < SECTOR_BYTES:
        chunk = dev.read_rom((sector_base + off) >> 1, 0x1000)
        if len(chunk) != 0x1000:
            dev.cmd(0xF3)
            sys.exit("short read at 0x%X. Is a cartridge inserted?"
                     % (sector_base + off))
        seen |= set(chunk)
        if seen != {0xFF}:
            dev.cmd(0xF3)
            sys.exit("sector 0x%X is NOT blank (differs at/near 0x%X). Refusing "
                     "to write: this test only ever touches erased space."
                     % (sector_base, sector_base + off))
        off += 0x1000
    ck(True, "sector 0x%X is erased across all %d KiB"
       % (sector_base, SECTOR_BYTES // 1024))

    print("\nFLASH_METHOD 1 programs and verifies")
    ck(dev.flash_cmds(0x01, 0x01, AMD_SINGLE) == b"\x01",
       "the AMD single-write template is accepted")
    dev.sv(2, 0x05, 0x80)              # STATUS_REGISTER_MASK
    dev.sv(2, 0x06, 0x80)              # STATUS_REGISTER_VALUE
    dev.sv(2, 0x00, LEN)                # TRANSFER_SIZE
    dev.sv(4, 0x00, hw)                 # ADDRESS

    payload = bytes(((i * 7) ^ 0x5A) & 0xFF for i in range(LEN))
    t0 = time.time()
    dev.d.reset_input_buffer()
    dev.d.write(bytes([0xD3]) + payload)
    dev.d.flush()
    ack = dev.d.read(1)
    dt = time.time() - t0
    ck(ack == b"\x01", "FLASH_PROGRAM acknowledges success", repr(ack))
    print("         %d bytes in %.3f s (%.1f kB/s)" % (LEN, dt, LEN / dt / 1000))

    after = dev.read_rom(hw, LEN)
    diffs = sum(1 for a, b in zip(after, payload) if a != b) if len(after) == LEN else LEN
    ck(diffs == 0, "every byte reads back as written", "%d bytes differ" % diffs)

    print("\na write that cannot take is an error, not a success")
    # Flash bits only go 1 -> 0 without an erase, so 0x0000 -> 0xFFFF is a
    # write the chip cannot perform.
    addr2 = hw + (LEN // 2)
    dev.sv(2, 0x00, 2)
    dev.sv(4, 0x00, addr2)
    dev.d.reset_input_buffer()
    dev.d.write(bytes([0xD3, 0x00, 0x00]))
    dev.d.flush()
    ck(dev.d.read(1) == b"\x01", "a word programs to 0x0000")

    dev.sv(4, 0x00, addr2)
    t0 = time.time()
    dev.d.reset_input_buffer()
    dev.d.write(bytes([0xD3, 0xFF, 0xFF]))
    dev.d.flush()
    ack = dev.d.read(1)
    dt = time.time() - t0
    ck(ack == b"\x02", "0x0000 -> 0xFFFF without an erase answers ACK_ERROR",
       repr(ack))
    ck(0.4 < dt < 1.2, "and takes the 500 ms budget, not a spin count",
       "%.3f s" % dt)
    sr = dev.gv(2, 0x03)                # STATUS_REGISTER
    ck(sr == 0x0000,
       "STATUS_REGISTER holds what the chip actually had",
       "0x%s" % ("%04X" % sr if sr is not None else "??"))

    print("\nFLASH_METHOD 2 still programs and verifies")
    hw2 = (sector_base + 0x1000) >> 1
    ck(dev.flash_cmds(0x01, 0x02, AMD_BUFFERED) == b"\x01",
       "the AMD buffered template is accepted")
    dev.sv(2, 0x01, 32)                 # BUFFER_SIZE, from the chip's CFI
    dev.sv(2, 0x00, LEN)
    dev.sv(4, 0x00, hw2)
    payload2 = bytes(((i * 11) ^ 0x3C) & 0xFF for i in range(LEN))
    t0 = time.time()
    dev.d.reset_input_buffer()
    dev.d.write(bytes([0xD3]) + payload2)
    dev.d.flush()
    ack = dev.d.read(1)
    dt = time.time() - t0
    ck(ack == b"\x01", "buffered FLASH_PROGRAM acknowledges success", repr(ack))
    print("         %d bytes in %.3f s (%.1f kB/s)" % (LEN, dt, LEN / dt / 1000))
    after2 = dev.read_rom(hw2, LEN)
    d2 = sum(1 for a, b in zip(after2, payload2) if a != b) if len(after2) == LEN else LEN
    ck(d2 == 0, "every buffered byte reads back as written", "%d bytes differ" % d2)

    print("\na failed buffer stops the block, it does not grind through the rest")
    # agb_status_wait() takes a fresh 500 ms budget per call: grinding on past
    # a failed buffer costs 64 x 500 ms = 32 s on a 2048-byte block, past the
    # host's DEVICE_TIMEOUT = 1 s (LK_Device.py:132). A failed AMD buffered
    # program also leaves the part in write-buffer-abort, which a plain
    # AA/55/25 unlock does not clear. LK.c:664/676 break on _timeout_check().
    hw3 = (sector_base + 0x2000) >> 1
    dev.sv(2, 0x00, LEN)
    dev.sv(4, 0x00, hw3)
    dev.d.reset_input_buffer()
    dev.d.write(bytes([0xD3]) + bytes(LEN))
    dev.d.flush()
    ck(dev.d.read(1) == b"\x01", "a block of 0x00 programs")

    dev.sv(4, 0x00, hw3)
    dev.d.reset_input_buffer()
    dev.d.write(bytes([0xD3]) + b"\xFF" * LEN)
    dev.d.flush()
    t0 = time.time()
    ack = dev.d.read(1)
    dt = time.time() - t0
    nbuf = LEN // 32
    ck(ack == b"\x02", "0x00 -> 0xFF answers ACK_ERROR", repr(ack))
    ck(dt < 1.2,
       "and stops after ONE 500 ms budget, not %d of them" % nbuf,
       "%.2f s. %d buffers x 500 ms is %.1f s, so it ground through them"
       % (dt, nbuf, nbuf * 0.5))

    print("\nrestoring the sector to erased")
    dev.sv(2, 0x05, 0x80)
    dev.sv(2, 0x06, 0x80)
    sector = sector_base >> 1
    for a, v in ((0xAAA >> 1, 0xAA), (0x555 >> 1, 0x55), (0xAAA >> 1, 0x80),
                 (0xAAA >> 1, 0xAA), (0x555 >> 1, 0x55), (sector, 0x30)):
        dev.bus_write(a, v)
    deadline = time.time() + 30
    blank = False
    while time.time() < deadline:
        time.sleep(0.25)
        chk = dev.read_rom(hw, 64)
        if len(chk) == 64 and set(chk) == {0xFF}:
            blank = True
            break
    ck(blank, "the sector erases back to 0xFF")

    dev.cmd(0xF3)                       # cartridge power off
    dev.d.close()
    print("\ntest_flash_program: %d passed, %d failed" % (_pass, _fail))
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
