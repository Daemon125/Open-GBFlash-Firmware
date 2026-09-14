#!/usr/bin/env python3
"""The four routed LK opcodes must refuse a payload larger than data_buffer.

0xB3, 0xB6, 0xB7 and 0xB9 each do an unchunked lk_conn_recv(data_buffer,
TRANSFER_SIZE). data_buffer is 0x1000; TRANSFER_SIZE is clamped only to
FW_MAX_TRANSFER because that cell is also the ROM read block size. In
the built image data_buffer + 4308 is the cartridge bus-mode byte and
data_buffer + 8604 is the end of SRAM, so an oversized payload overwrites
firmware state and then runs off the end of RAM. Stock FlashGBX sets
TRANSFER_SIZE <= 0x800 first, so it never triggers it.

lk_admit() refuses and drains rather than truncating: a short write to a save
chip is a corrupted save, so the host has to learn the command did not happen.

The cartridge is never powered here, so a failing firmware scribbles on its own
state rather than on a cartridge.
"""
import glob, struct, sys, time
try:
    import serial
except ImportError:
    sys.exit("pyserial required")

OPS = [(0xB3, "DMG_CART_WRITE_SRAM"), (0xB6, "DMG_MBC7_WRITE_EEPROM"),
       (0xB7, "DMG_MBC6_MMSA_WRITE_FLASH"), (0xB9, "DMG_EEPROM_WRITE")]
DATA_BUFFER = 0x1000

ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")
               + glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
if not ports:
    sys.exit("no device")
d = serial.Serial(ports[0], 2000000, timeout=5)
time.sleep(0.4)
while d.read(65536):
    pass


def cmd(b, n=0):
    d.write(b if isinstance(b, (bytes, bytearray)) else bytes([b]))
    d.flush()
    return d.read(n) if n else b""


def sv(size, key, val):
    return cmd(bytes([0xA6, size]) + struct.pack(">I", key)
               + struct.pack(">I", val), 1)


def ident():
    d.reset_input_buffer()
    d.write(bytes([0xA1]))
    d.flush()
    if d.read(1) != bytes([8]):
        return None
    i = d.read(8)
    ln = d.read(1)[0]
    nm = d.read(ln)
    d.read(2)
    return "%s%d PCB %d %r" % (chr(i[0]), int.from_bytes(i[1:3], "big"), i[3],
                               nm.decode("ascii", "replace"))


who = ident()
if who is None:
    sys.exit("device is not answering the protocol")
print("device: %s\n" % who)

cmd(0xA3, 1)                                  # DMG mode, cartridge stays off

passed = failed = 0
for op, name in OPS:
    sv(2, 0x00, DATA_BUFFER + 0x800)          # TRANSFER_SIZE = 0x1800
    sv(4, 0x00, 0xA000)
    d.reset_input_buffer()
    d.write(bytes([op]))
    d.flush()
    d.write(b"\xA5" * (DATA_BUFFER + 0x800))
    d.flush()
    ack = d.read(1)
    alive = ident()
    ok = (ack == bytes([0x02])) and (alive == who)
    print("  %-28s oversized -> ack %-6s device %s   %s"
          % (name, ack.hex() if ack else "none",
             "alive" if alive == who else "LOST",
             "ok" if ok else "FAIL"))
    passed += ok
    failed += not ok

sv(2, 0x00, 0x40)
sv(4, 0x00, 0xA000)
d.reset_input_buffer()
d.write(bytes([0xB3]))
d.flush()
d.write(b"\x00" * 0x40)
d.flush()
ack = d.read(1)
alive = ident()
ok = (ack is not None and len(ack) == 1) and (alive == who)
print("  %-28s legal 0x40 -> ack %-6s device %s   %s"
      % ("DMG_CART_WRITE_SRAM", ack.hex() if ack else "none",
         "alive" if alive == who else "LOST", "ok" if ok else "FAIL"))
passed += ok
failed += not ok

print("\ntest_lk_payload_bound: %d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
