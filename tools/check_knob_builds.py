#!/usr/bin/env python3
"""Build every boolean knob at its non-default setting and report what breaks.

A knob that reaches CFLAGS still ships a broken configuration if only one of its
two arms compiles. FW_DMG_WRITE_BURST=0 failed to link for weeks that way: the
functions its off path needs are defined under its own #if, and an independent
knob gated the caller. host/test_stale_claims.py cannot see this, because the
-D is present and the claim text is true. Only building both arms finds it.

    tools/check_knob_builds.py              every boolean knob, both arms
    tools/check_knob_builds.py --only DMG   knobs whose name contains DMG
    tools/check_knob_builds.py --jobs 4     parallel, each in its own BUILD dir

Needs the cross toolchain. Not part of `make -C host`, which is toolchain-free.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Toggling these does not describe a shipping configuration. Only names whose
# Makefile default is literally 0 or 1 can reach here: knobs() filters on that,
# so a knob with a non-boolean default needs no entry.
SKIP = {
    "FW_READ_FAST",
    # Deliberately breaks the descriptor set; usb_desc.c #warnings on purpose.
    "FW_USB_CDC_BREAK",
}

# Pairs that cannot both be set. Building one arm of these is not a defect.
CONFLICTS = [
    ("FW_AGB_LEAF", "FW_STREAM_MIDGROUP_POLL"),
]


def knobs():
    """Each knob once. A name defined twice would build twice into one BUILD
    directory, and the two makes race through the Makefile's `rm -f $(BUILD)/*.o'.
    """
    text = open(os.path.join(ROOT, "Makefile"), encoding="utf-8").read()
    out, seen, dupes = [], set(), []
    for m in re.finditer(r"^([A-Z][A-Z0-9_]*)\s*\?=\s*([01])\s*$", text, re.M):
        name, default = m.group(1), m.group(2)
        if name in SKIP:
            continue
        if name in seen:
            dupes.append(name)
            continue
        seen.add(name)
        out.append((name, default))
    if dupes:
        sys.exit("check_knob_builds: %s defined more than once in the Makefile; "
                 "the second %s a no-op under ?= and splits this check across "
                 "two builds of one directory"
                 % (", ".join(sorted(set(dupes))),
                    "is" if len(set(dupes)) == 1 else "are"))
    return out


def conflicts_with_default(name, value, defaults):
    for a, b in CONFLICTS:
        other = b if name == a else (a if name == b else None)
        if other is None:
            continue
        mine = value
        theirs = defaults.get(other)
        if mine == "1" and theirs == "1":
            return other
    return None


def build(args_list, builddir):
    cmd = ["make", "-C", ROOT, "BUILD=" + builddir] + args_list
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr)


def classify(log):
    """A build refused by #error is correct behaviour, not a break.

    The knob says no and says why. A break is an undefined reference or any
    other diagnostic, which means one arm was never compiled.
    """
    has_error_directive = bool(re.search(r"#error", log))
    other = re.search(r"undefined reference|implicit declaration|"
                      r"error: (?!#error)", log)
    if has_error_directive and not other:
        m = re.search(r'#error\s+"([^"]+)"', log)
        return "refused", (m.group(1) if m else "refused by #error")
    return "fail", "\n".join(summarise(log))


def summarise(log):
    bad = []
    for line in log.split("\n"):
        low = line.lower()
        if "undefined reference" in low or "error:" in low or "#error" in low:
            bad.append(line.strip()[:150])
    if not bad:
        for line in log.split("\n"):
            if "Error" in line and "make" in line:
                bad.append(line.strip()[:150])
    seen, out = set(), []
    for b in bad:
        if b not in seen:
            seen.add(b)
            out.append(b)
    return out[:4]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--only", help="substring filter on the knob name")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    ks = knobs()
    defaults = dict(ks)
    if args.only:
        ks = [(n, d) for n, d in ks if args.only.upper() in n]
    if not ks:
        print("no knobs matched")
        return 0

    print("check_knob_builds: %d boolean knobs, each flipped from its default"
          % len(ks))
    tmp = tempfile.mkdtemp(prefix="knobcheck.")
    failures, skipped = [], []

    def one(item):
        name, default = item
        flipped = "0" if default == "1" else "1"
        clash = conflicts_with_default(name, flipped, defaults)
        if clash:
            return (name, flipped, "skip", clash)
        bd = os.path.join(tmp, name)
        rc, log = build(["%s=%s" % (name, flipped)], bd)
        shutil.rmtree(bd, ignore_errors=True)
        if rc == 0:
            return (name, flipped, "ok", "")
        state, detail = classify(log)
        return (name, flipped, state, detail)

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        results = list(ex.map(one, ks))

    refused = []
    for name, val, state, detail in sorted(results):
        if state == "ok":
            continue
        if state == "skip":
            skipped.append((name, val, detail))
        elif state == "refused":
            refused.append((name, val, detail))
        else:
            failures.append((name, val, detail))
    for name, val, why in refused:
        print("  [refused] %s=%s: %s" % (name, val, why))

    for name, val, other in skipped:
        print("  [skip] %s=%s conflicts with %s at its default" % (name, val, other))
    for name, val, detail in failures:
        print("  [FAIL] %s=%s does not build" % (name, val))
        for line in detail.split("\n"):
            if line:
                print("         %s" % line)

    shutil.rmtree(tmp, ignore_errors=True)
    ok = len(results) - len(failures) - len(skipped) - len(refused)
    print("\ncheck_knob_builds: %d built, %d refused by #error, %d skipped, "
          "%d failed" % (ok, len(refused), len(skipped), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
