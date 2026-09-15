#!/usr/bin/env python3
"""hw_GBFlash.py carries copies of two LK_Device methods. Notice when they move.

ReadROM and _try_write are overridden by reimplementation, not by delegation.
If FlashGBX changes either one, our copies keep the old behaviour silently, and
the first symptom is a bad dump. Fail here instead.

Update the hash in the same commit as the port; alone it turns the check green
over a stale copy.

The other overrides delegate, so only their signatures matter.
"""
import hashlib, inspect, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
for extra in (os.path.abspath(os.path.join(ROOT, "..", "..", "FlashGBX-stock")),
              os.path.abspath(os.path.join(ROOT, "..", "..", "FlashGBX"))):
    if os.path.isdir(os.path.join(extra, "FlashGBX")) and extra not in sys.path:
        sys.path.append(extra)

# sha256 of inspect.getsource() for the upstream revision our copies were taken
# from. FlashGBX d289b2f, LK_Device.py as of 2026-09-14.
COPIED = {
    "ReadROM":    "fb2b2d49fc3e41bde0a083ed6af36a853bf86e5c0c0a147aba8ad2c470d49902",
    "_try_write": "ab679983c353fe0a704e958cf6391140ea8a124a3cacecc314b977aa75b05478",
}
# Delegated: a changed signature breaks the call, a changed body does not.
DELEGATED = {
    "SetAGBReadMethod": "(self, method)",
    "_set_fw_variable": "(self, key, value)",
}


def main():
    print("test_upstream_drift: our copies of LK_Device methods must stay current")
    try:
        from FlashGBX.LK_Device import LK_Device
    except ImportError as e:
        print("  skipped, FlashGBX is not importable here (%s)" % e)
        return 0
    print("  checking %s" % inspect.getfile(LK_Device))
    fails = 0

    for name, want in COPIED.items():
        fn = getattr(LK_Device, name, None)
        if fn is None:
            print("  [FAIL] LK_Device.%s no longer exists" % name); fails += 1; continue
        got = hashlib.sha256(inspect.getsource(fn).encode()).hexdigest()
        if got != want:
            print("  [FAIL] LK_Device.%s changed upstream" % name)
            print("         recorded %s" % want)
            print("         current  %s" % got)
            print("         re-diff it against our override in hw_GBFlash.py,")
            print("         port what changed, then update the hash here.")
            fails += 1
        else:
            print("  %-18s unchanged" % name)

    for name, want in DELEGATED.items():
        fn = getattr(LK_Device, name, None)
        if fn is None:
            print("  [FAIL] LK_Device.%s no longer exists" % name); fails += 1; continue
        got = str(inspect.signature(fn))
        if got != want:
            print("  [FAIL] LK_Device.%s signature is now %s, we delegate as %s"
                  % (name, got, want))
            fails += 1
        else:
            print("  %-18s signature unchanged" % name)

    print("test_upstream_drift: %s" % ("FAILED" if fails else "passed"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
