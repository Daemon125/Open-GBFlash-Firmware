#!/usr/bin/env bash
# Build and install the open firmware over USB, no jumper and no button.
#
#   tools/flash.sh              build, reset to bootloader, flash, verify
#   tools/flash.sh --stock      put the vendor L15 firmware back
#
# HAZARD: an image that does not answer BOOTLOADER_RESET (0xF1) breaks the USB
# reflash loop. Recovery is U22 held at power-on.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"

# Checked before the handover below, not after: that sends BOOTLOADER_RESET,
# and failing once the board is in update mode leaves nothing able to finish.
UPDATER="${GBFLASH_UPDATER:-../ref/gbflash_unlocker/gbflash_serial_update.py}"
if [ ! -f "$UPDATER" ]; then
    echo "  missing: $UPDATER" >&2
    echo "  It belongs to the GBFlash Unlocker project, which this repository" >&2
    echo "  does not carry. Point GBFLASH_UPDATER at it, or install the image" >&2
    echo "  through the FlashGBX firmware updater instead." >&2
    exit 1
fi

IMAGE="build/fw.bin"
if [ "${1:-}" = "--stock" ]; then
    IMAGE="../fw/fw.bin"
    echo "  restoring vendor firmware: $IMAGE"
elif [ "${1:-}" = "--image" ]; then
    IMAGE="${2:?usage: flash.sh --image <fw.bin>}"
    echo "  flashing image: $IMAGE"
else
    # Plain `make', so a knob build made just before this is REBUILT AT THE
    # DEFAULTS and the knob image is discarded. Use --image to flash what you
    # just built. A whole day of A/B measurements was once taken against the
    # default image because of this.
    make --no-print-directory
fi

# Also before the handover: a missing or empty image strands the board in
# update mode exactly as a missing updater does.
if [ ! -s "$IMAGE" ]; then
    echo "  missing or empty image: $IMAGE" >&2
    echo "  Not touching the device." >&2
    exit 1
fi

if command -v md5 >/dev/null 2>&1; then
    echo "  image md5: $(md5 -q "$IMAGE")"
elif command -v md5sum >/dev/null 2>&1; then
    echo "  image md5: $(md5sum "$IMAGE" | cut -d' ' -f1)"
fi

python3 - "$IMAGE" <<'PY'
import glob, sys, time
try:
    import serial
except ImportError:
    sys.exit("needs pyserial: python3 -m pip install pyserial")

# The bootloader is always a CH340, /dev/cu.usbserial-* (wchusbserial under
# WCH's own driver). An FW_USB_CDC app is /dev/cu.usbmodem*. Drop usbmodem from
# this list and a CDC app cannot be asked to hand over, leaving U22 at power-on
# as the only way in.
ports = sorted(glob.glob('/dev/cu.usbserial*') + glob.glob('/dev/cu.wchusbserial*')
               + glob.glob('/dev/cu.usbmodem*') + glob.glob('/dev/ttyUSB*'))
if not ports:
    sys.exit("no device found")
port = ports[0]

d = serial.Serial(port, 2000000, timeout=1)
time.sleep(0.25)
d.reset_input_buffer()
d.write(bytes([0xA1])); d.flush()
if d.read(1)[:1] == b'':
    d.close()
    sys.exit("no answer to QUERY_FW_INFO. Already in update mode? "
             "Run the updater with --skip-trigger")

d.reset_input_buffer()
d.write(bytes([0xF1])); d.flush()
ack = d.read(1)
if ack[:1] not in (b'\x01', b'\x03'):
    d.close()
    sys.exit("this firmware does not answer BOOTLOADER_RESET (%r).\n"
             "Hold U22 while plugging in, then run the updater with "
             "--skip-trigger." % ack)
d.write(bytes([0x01])); d.flush()
d.close()
print("  handed over to the bootloader")
# Fixed sleep, not a wait for the port to vanish: on a stock-identity build app
# and bootloader are both a CH340 under the same name, so that wait never
# returns. Under FW_USB_CDC the handover is a real re-enumeration, app 1209:0001
# on usbmodem to bootloader 1A86:7523 on usbserial; gbflash_serial_update.py
# finds the bootloader by VID/PID either way.
time.sleep(3.0)
PY

python3 "$UPDATER" "$IMAGE" --skip-trigger
sleep 3

python3 - <<'PY'
import glob, sys, time, serial
ports = sorted(glob.glob('/dev/cu.usbserial*') + glob.glob('/dev/cu.wchusbserial*')
               + glob.glob('/dev/cu.usbmodem*'))
if not ports:
    sys.exit("device did not come back on USB")
print("  came back as %s" % ports[0])
d = serial.Serial(ports[0], 2000000, timeout=2)
time.sleep(0.25)
d.reset_input_buffer(); d.write(bytes([0xA1])); d.flush()
n = d.read(1)
if len(n) != 1 or n[0] == 0xA1:
    d.close()
    sys.exit("came back but is not answering the protocol (echoing?)")
info = d.read(8); ln = d.read(1)[0]; name = d.read(ln); d.read(2)
print("  running: %s%d, PCB %d, %r"
      % (chr(info[0]), int.from_bytes(info[1:3], 'big'), info[3],
         name.decode('ascii', 'replace')))
d.close()
PY
