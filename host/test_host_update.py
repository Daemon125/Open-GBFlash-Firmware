#!/usr/bin/env python3
"""Check what hw_GBFlash.py offers as a firmware update.

The device reports its own build date now instead of copying the stock one, so
the vendor table it used to be compared against says nothing about it. An
Open-GBFlash device is compared against res/fw_Open-GBFlash.zip; a stock device
must still get upstream's answer, byte for byte.

Needs the FlashGBX package importable. A clone does not carry it, so this skips
rather than fails when it is absent.
"""

import hashlib
import io
import os
import shutil
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# Parents of a FlashGBX package directory, so "import FlashGBX" can find one.
CANDIDATES = (
    os.path.abspath(os.path.join(ROOT, "..", "..", "FlashGBX-stock")),
    os.path.abspath(os.path.join(ROOT, "..", "..", "FlashGBX")),
)

DEV_TS = 1788313704          # what the firmware reports
STOCK_TS = 1780508702        # DEVICE_LATEST_FW_TS, the stock build date
CUTOFF = 1730592000          # below this upstream calls the firmware unofficial

_pass = _fail = 0


def ck(label, got, want):
    global _pass, _fail
    if got == want:
        _pass += 1
    else:
        _fail += 1
        print("  [FAIL] %s: got %r, want %r" % (label, got, want))


def main():
    print("test_host_update: the update offer must suit the firmware that is running")
    for extra in CANDIDATES:
        if os.path.isdir(os.path.join(extra, "FlashGBX")) and extra not in sys.path:
            sys.path.append(extra)
    try:
        from FlashGBX.app import AppContext
        from FlashGBX import hw_GBFlash
        from FlashGBX.hw_GBFlash import GbxDevice
    except ImportError as e:
        print("  skipped, FlashGBX is not importable here (%s)" % e)
        return 0

    # An out-of-date copy would test something this repo does not ship.
    def digest(path):
        with open(path, "rb") as f:
            return hashlib.md5(f.read()).hexdigest()
    loaded = os.path.abspath(hw_GBFlash.__file__)
    mine = os.path.join(ROOT, "hw_GBFlash.py")
    if digest(loaded) != digest(mine):
        print("  [FAIL] imported %s differs from %s" % (loaded, mine))
        return 1
    print("  testing %s" % loaded)

    app = tempfile.mkdtemp()
    cfg = tempfile.mkdtemp()
    os.makedirs(os.path.join(app, "res"))
    AppContext.APP_PATH = app
    AppContext.CONFIG_PATH = cfg
    zip_path = os.path.join(app, "res", "fw_Open-GBFlash.zip")

    def put(ts, valid=True, ini=None, break_payload=False):
        if os.path.exists(zip_path):
            os.remove(zip_path)
        if not valid:
            with open(zip_path, "wb") as f:
                f.write(b"not a zip")
            return
        text = ini if ini is not None else (
            "[Firmware]\nfw_ver = L15\nfw_buildts = %s\n" % ts)
        if not break_payload:
            with zipfile.ZipFile(zip_path, "w") as z:
                z.writestr("fw.bin", b"\x00" * 16)
                z.writestr("fw.ini", text)
            return
        # A damaged member passes a central-directory lookup and only fails when
        # the bytes are read, which WriteFirmware does after the device is
        # already in the bootloader.
        raw = io.BytesIO()
        with zipfile.ZipFile(raw, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("fw.bin", os.urandom(4096))
            z.writestr("fw.ini", text)
        b = bytearray(raw.getvalue())
        hdr = b.find(b"PK\x03\x04")
        b[hdr + 40:hdr + 70] = bytes(30)
        with open(zip_path, "wb") as f:
            f.write(bytes(b))

    def dev(open_fw, ts=DEV_TS):
        d = GbxDevice()
        d.OPEN_FW = open_fw
        d.FW = {"fw_ts": ts, "pcb_ver": 13,
                "pcb_name": "Open-GBFlash" if open_fw else "GBFlash"}
        return d

    print("\n1. an Open-GBFlash device is compared against the zip beside it")
    put(DEV_TS + 86400); ck("zip newer", dev(True).FirmwareUpdateAvailable(), True)
    put(DEV_TS);         ck("zip same", dev(True).FirmwareUpdateAvailable(), False)
    put(DEV_TS - 86400); ck("zip older", dev(True).FirmwareUpdateAvailable(), False)

    print("\n2. an unusable zip offers nothing rather than raising")
    put(0, valid=False);  ck("corrupt zip", dev(True).FirmwareUpdateAvailable(), False)
    put("notanumber");    ck("fw_buildts not a number", dev(True).FirmwareUpdateAvailable(), False)
    put(2 ** 70);         ck("fw_buildts out of range", dev(True).FirmwareUpdateAvailable(), False)
    put(0, break_payload=True)
    ck("fw.bin damaged inside the zip", dev(True).FirmwareUpdateAvailable(), False)
    put(0, ini="this has no section header\n")
    ck("fw.ini has no section header", dev(True).FirmwareUpdateAvailable(), False)
    put(0, ini="[Firmware]\nfw_ver\n")
    ck("fw.ini key with no separator", dev(True).FirmwareUpdateAvailable(), False)
    os.remove(zip_path);  ck("no zip at all", dev(True).FirmwareUpdateAvailable(), False)

    print("\n3. the prompt has an off switch, since upstream's is never rendered")
    put(DEV_TS + 86400)
    ini = os.path.join(cfg, "settings.ini")
    with open(ini, "w") as f:
        f.write("[General]\nSkipOpenFirmwareUpdate = enabled\n")
    ck("opted out", dev(True).FirmwareUpdateAvailable(), False)
    os.remove(ini)

    print("\n4. a stock device still gets upstream's answer")
    ck("equal to the table", dev(False, STOCK_TS).FirmwareUpdateAvailable(), False)
    ck("differs from the table", dev(False, STOCK_TS + 1).FirmwareUpdateAvailable(), True)
    d = dev(False, CUTOFF - 1)
    ck("below the cutoff is a required update",
       (d.FirmwareUpdateAvailable(), d.FW_UPDATE_REQ), (True, True))

    print("\n5. no Open-GBFlash device is ever told an update is required")
    for ts in (CUTOFF - 1, STOCK_TS, DEV_TS):
        d = dev(True, ts)
        d.FirmwareUpdateAvailable()
        ck("fw_ts=%d leaves FW_UPDATE_REQ clear" % ts, d.FW_UPDATE_REQ, False)

    shutil.rmtree(app)
    shutil.rmtree(cfg)
    print("\ntest_host_update: %d passed, %d failed" % (_pass, _fail))
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
