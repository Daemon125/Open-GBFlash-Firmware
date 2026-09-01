#!/usr/bin/env python3
"""Install a firmware image over USB. Windows, macOS and Linux.

HAZARD: an image that does not answer BOOTLOADER_RESET (0xF1) breaks the USB
reflash loop. Recovery is the U22 button held at power-on.
"""

import os
import subprocess
import sys
import time

import serial
import serial.tools.list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
BOOTLOADER = (0x1A86, 0x7523)      # CH340, and every stock-identity firmware
OPEN_CDC = (0x1209, 0x0008)        # Open-GBFlash's own CDC identity


def ports(*ids):
    return [p.device for p in serial.tools.list_ports.comports()
            if (p.vid, p.pid) in ids]


def find(timeout=15.0, ids=(BOOTLOADER, OPEN_CDC)):
    end = time.time() + timeout
    while time.time() < end:
        found = ports(*ids)
        if found:
            return sorted(found)[0]
        time.sleep(0.5)
    return None


def send(d, data, settle=0.0):
    """Write and make sure it actually left the machine before moving on.

    flush() drains to the driver, which on POSIX means tcdrain and on Windows
    means polling out_waiting. Neither guarantees the bytes have crossed the
    USB pipe, and closing the port immediately after a write can lose them. The
    byte at risk here is the one that makes the device jump to the bootloader:
    lose it and the handover silently does nothing, which looks exactly like the
    command was never sent.
    """
    d.write(data)
    d.flush()
    try:
        end = time.time() + 2.0
        while d.out_waiting and time.time() < end:
            time.sleep(0.02)
    except (OSError, AttributeError, NotImplementedError):
        pass
    if settle:
        time.sleep(settle)


def query_fw_info(d):
    """Send QUERY_FW_INFO and consume its WHOLE reply. Returns the name, or None.

    The reply is a status byte, 8 info bytes, a length, that many name bytes and
    a 2 byte tail. Reading only the status byte leaves the rest in flight, and
    reset_input_buffer() purges what has ALREADY arrived, not what is still
    coming. On Windows the remainder lands after the purge and the next read
    returns a leftover byte: BOOTLOADER_RESET was answered with 0x6A, a piece of
    the previous reply, and the handover was refused as if the firmware had
    rejected it. macOS delivers the reply fast enough that the purge catches all
    of it, which is why this only ever appeared on Windows.
    """
    d.reset_input_buffer()
    send(d, bytes([0xA1]))
    first = d.read(1)
    if first[:1] in (b"", b"\x00"):
        return None
    info = d.read(8)
    ln = d.read(1)
    if len(info) != 8 or len(ln) != 1:
        return None
    name = d.read(ln[0])
    d.read(2)
    drain(d)
    return name


def drain(d, quiet=0.15, limit=1.5):
    """Read until nothing more arrives, so no byte outlives its command."""
    end = time.time() + limit
    while time.time() < end:
        if not d.read(64):
            time.sleep(quiet)
            if not d.in_waiting:
                return
    return


def in_bootloader():
    """True when a CH340 is present and it is NOT running app firmware.

    Identity alone cannot answer this. Open-GBFlash is 1209:0008 and its
    bootloader is 1A86:7523, but stock firmware is 1A86:7523 as well, so the
    port looks the same before and after. What differs is behaviour: the
    bootloader does not answer QUERY_FW_INFO.
    """
    found = ports(BOOTLOADER)
    if not found:
        return False
    try:
        d = serial.Serial(sorted(found)[0], 2000000, timeout=0.8)
    except Exception:
        return False
    try:
        time.sleep(0.15)
        return query_fw_info(d) is None
    except Exception:
        return False
    finally:
        d.close()


def resync(d):
    """Recover a firmware left part way through a command.

    An interrupted transfer leaves the command parser waiting for argument
    bytes, so every later command is consumed as arguments and the device looks
    dead. Feeding it more zeros than any command takes, then draining, puts the
    parser back at a command boundary. This recovered a device that answered
    neither QUERY_FW_INFO nor the bootloader and looked like it needed the U22
    button.
    """
    for _ in range(4):
        send(d, b"\x00" * 2048)
        time.sleep(0.4)
        while d.read(65536):
            pass
        d.reset_input_buffer()
        send(d, bytes([0xA1]))
        r = d.read(1)
        if r[:1] not in (b"", b"\x00"):
            return r
        time.sleep(0.5)
    return b""


def describe_ports():
    out = []
    for p in serial.tools.list_ports.comports():
        vid = "%04X" % p.vid if p.vid is not None else "????"
        pid = "%04X" % p.pid if p.pid is not None else "????"
        out.append("%s  %s:%s  %s" % (p.device, vid, pid, p.description or ""))
    return out


def wait_for_bootloader(timeout=30.0):
    """Wait for the CH340 bootloader to enumerate after the handover.

    The old code slept three seconds and hoped. That is enough on macOS, where
    the port comes back almost at once. Windows has to unbind the old device,
    bind the CH340 driver and assign a COM number, and if the updater runs
    before that finishes it simply finds nothing.
    """
    end = time.time() + timeout
    while time.time() < end:
        if ports(BOOTLOADER):
            time.sleep(0.6)          # let the COM port settle once it appears
            return True
        time.sleep(0.4)
    return False


def handover():
    """Ask a running firmware to jump to the bootloader, and check that it did.

    Verified rather than assumed. The confirm byte can be lost on close, and a
    lost confirm leaves the device happily running its application firmware
    with nothing to show that anything went wrong.
    """
    for attempt in range(1, 4):
        if in_bootloader():
            print("  already in the bootloader")
            return True
        port = find()
        if port is None:
            print("  no GBFlash found on USB. Looking for 1A86:7523 (bootloader")
            print("  or stock firmware) or 1209:0008 (Open-GBFlash).")
            print("  Serial ports currently present:")
            for line in describe_ports() or ["    (none at all)"]:
                print("    %s" % line)
            print()
            print("  If the device is plugged in and nothing is listed, Windows")
            print("  has no driver bound to it. The bootloader is a CH340:")
            print("  install the WCH CH341SER driver, then try again.")
            return False
        try:
            d = serial.Serial(port, 2000000, timeout=1)
        except Exception as e:
            print("  could not open %s: %s" % (port, e))
            return False
        try:
            time.sleep(0.25)
            if query_fw_info(d) is None:
                print("  no clean answer to QUERY_FW_INFO; resynchronising")
                if resync(d)[:1] == b"":
                    print("  no answer even after a resync")
                    return False
                drain(d)
            d.reset_input_buffer()
            send(d, bytes([0xF1]))
            ack = d.read(1)
            if ack[:1] not in (b"\x01", b"\x03", b""):
                # A stale byte from an earlier reply reads as a refusal. Clear
                # the line and ask once more before believing it.
                drain(d)
                d.reset_input_buffer()
                send(d, bytes([0xF1]))
                ack = d.read(1)
            if ack[:1] not in (b"\x01", b"\x03"):
                print("  this firmware does not answer BOOTLOADER_RESET (%r)" % ack)
                print("  Hold U22 while plugging in, then run again.")
                return False
            # settle before close: this is the byte that triggers the jump
            send(d, bytes([0x01]), settle=0.4)
        finally:
            d.close()

        end = time.time() + 12.0
        while time.time() < end:
            if in_bootloader():
                print("  handed over to the bootloader")
                return True
            time.sleep(0.4)
        print("  handover attempt %d did not reach the bootloader" % attempt)
        time.sleep(1.5)

    print("  the device stayed on its application firmware after 3 attempts.")
    print("  Hold U22 while plugging it in, then run this again.")
    return False


def identify():
    port = find(timeout=25.0)
    if port is None:
        return "device did not come back on USB"
    for _ in range(6):
        try:
            d = serial.Serial(port, 2000000, timeout=2)
            break
        except serial.SerialException:
            time.sleep(1.0)
    else:
        return "came back at %s but would not open" % port
    time.sleep(0.25)
    d.reset_input_buffer()
    d.write(bytes([0xA1]))
    d.flush()
    n = d.read(1)
    if len(n) != 1 or n[0] == 0xA1:
        d.close()
        return "came back but is not answering the protocol"
    info = d.read(8)
    ln = d.read(1)[0]
    name = d.read(ln)
    d.read(2)
    d.close()
    return "%s%d, PCB %d, %r" % (chr(info[0]),
                                 int.from_bytes(info[1:3], "big"), info[3],
                                 name.decode("ascii", "replace"))


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: flash_fw.py <fw.bin>")
    image = sys.argv[1]
    if not os.path.exists(image):
        sys.exit("no such image: %s" % image)
    if not handover():
        sys.exit("could not put the device into update mode")
    if not wait_for_bootloader():
        print("  the bootloader did not appear on USB within 30 s")
        print("  ports currently present:")
        for line in describe_ports() or ["    (none)"]:
            print("    %s" % line)
        sys.exit("could not reach the bootloader")

    updater = os.path.join(HERE, "gbflash_serial_update.py")
    last = ""
    for attempt in range(1, 4):
        r = subprocess.run([sys.executable, updater, image, "--skip-trigger"],
                           capture_output=True, text=True, errors="replace")
        last = (r.stdout or "") + (r.stderr or "")
        if r.returncode == 0:
            break
        # The bootloader can enumerate a moment before it will talk, and on
        # Windows the COM number can change underneath the first attempt.
        print("  update attempt %d failed, retrying" % attempt)
        time.sleep(2.5)
        wait_for_bootloader(timeout=15.0)
    else:
        sys.stdout.write(last)
        print("  ports currently present:")
        for line in describe_ports() or ["    (none)"]:
            print("    %s" % line)
        sys.exit("firmware update failed after 3 attempts")
    time.sleep(3.0)
    print("  running: %s" % identify())


if __name__ == "__main__":
    main()
