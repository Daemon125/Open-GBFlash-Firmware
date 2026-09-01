#!/usr/bin/env python3
"""Keep the vendored LK firmware upgradable: pin it, prove it is pristine, diff its HAL.

Upgrading upstream (github.com/Lesserkuma/FlashGBX_LK_Firmware) stays a
three-file swap only while LK.c and LK.h are read-only. Changes go in our own
LK_device_*.h, in the transport, or upstream. Hashing enforces that.

The HAL contract is not only LK_device.h's macros. LK.c also reaches for hooks
behind `#ifdef`; a port that omits one still compiles, links, runs, and does
nothing where upstream intended something. In the pinned release:

    CART_POWER_ON_FIX_1        LK.c:864, inside the DMG power-on sequence
    CART_POWER_ON_FIX_2        LK.c:879
    CART_PRESENCE_SWITCH_GET   LK.c:815   (commented out in the template)
    CART_MODE_SWITCH_GET       LK.c:820   (commented out in the template)

Do not decide "used by upstream" by shape. Matching `[A-Z][A-Z0-9_]{2,}\\s*\\(`
while skipping `LK_*` hides 19 of the 69 macros LK.c/LK.h reference: call syntax
misses bare values (CHUNK_MAX_LEN, LK.c:1133), the leading [A-Z] misses every
lower-case one (all the delays, the whole _timeout_* mechanism), and the LK_
skip eats LK_DEVICE_NAME, LK_PCB_VERSION and the four support flags. A port
missing all the delays still builds and produces a plausible but wrong dump.

Proves a macro is implemented, not that it is implemented correctly. That is
what the host suite under host/ covers after a bump.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
DEFAULT_LK = os.path.join(FW, "upstream", "FlashGBX_LK_Firmware")
PIN_FILE = os.path.join(FW, "vendor", "lk_upstream.json")

VENDORED = ("LK.c", "LK.h", "LK_device.h")

# Include guards such as `_LK_DEVICENAME_H_` are not HAL obligations.
GUARD_RE = re.compile(r"^_.*_H_?$")

_pass = _fail = _warn = 0


def ck(cond, what, detail=""):
    global _pass, _fail
    if cond:
        _pass += 1
        print("  [ ok ] %s" % what)
    else:
        _fail += 1
        print("  [FAIL] %s%s" % (what, ("\n         " + detail) if detail else ""))
    return cond


def warn(what, detail=""):
    global _warn
    _warn += 1
    print("  [warn] %s%s" % (what, ("\n         " + detail) if detail else ""))


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def strip_code(src):
    """Remove comments and string/char literals, keeping everything else in place.

    Comments become a space, not nothing: `PIN_WR/**/_H` must not weld into a
    token absent from the source. Literals collapse to empty quotes so dprint()
    text stops feeding capitalised words to the identifier scan.
    """
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    src = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', src)
    src = re.sub(r"'(?:\\.|[^'\\\n])*'", "''", src)
    return src


def word(name):
    """Whole-identifier match for `name`. PIN_WR must not match PIN_WR_H."""
    return re.compile(r"(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])" % re.escape(name))


def define_names(text):
    return set(re.findall(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)", text, re.M))


def template_macros(text):
    """Live and commented-out declarations. A commented `#define` in the
    template is still a declared obligation."""
    live = define_names(text)
    commented = set(re.findall(
        r"^[ \t]*//[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)", text, re.M))
    return live, commented


def macro_bodies(text):
    """name -> replacement text, for the #defines in a device header.

    A template macro can be required without LK.c ever naming it: TIMESTAMP_NOW
    is reached only through _timeout_init/_timeout_check (LK_device.h:34,36),
    and a port that skips it does not build.
    """
    bodies = {}
    # Join backslash continuations, or a multi-line body reads as an empty one.
    joined = re.sub(r"\\[ \t]*\n", " ", text)
    for m in re.finditer(
            r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(\([^)]*\))?(.*)$", joined, re.M):
        bodies[m.group(1)] = m.group(3)
    return bodies


def used_macros(src, names):
    """Which of `names` appear in `src`, as a call or as a bare value.

    No shape or prefix heuristic: the candidate set is exactly the template's
    own declarations, so a wider match cannot invent a macro, only stop missing
    one.
    """
    return {n for n in names if word(n).search(src)}


def close_over_bodies(seed, bodies, names):
    """Grow `seed` with template macros that other required ones use."""
    out = set(seed)
    while True:
        grown = set()
        for n in out:
            for k in used_macros(strip_code(bodies.get(n, "")), names):
                if k not in out:
                    grown.add(k)
        if not grown:
            return out
        out |= grown


def invoked_names(src):
    return set(re.findall(r"(?<![A-Za-z0-9_])([A-Za-z_]\w*)[ \t]*\(", src))


def included_names(src):
    """Identifiers used as an #include operand. A build-time knob for which
    header to pull in (LK.h:17-18) is not a HAL macro."""
    return set(re.findall(r"^[ \t]*#[ \t]*include[ \t]+([A-Za-z_]\w*)", src, re.M))


def ifdef_hooks(src, own):
    """Optional hooks: anything LK.c guards with #ifdef and then uses. Missing
    a required macro is a build error; missing one of these is silence.

    `#ifndef` is excluded: include guards and LK.h's own fallbacks for
    NULL/bool/u8/s8/true, not board hooks. Names LK.c/LK.h define themselves
    and #include operands are subtracted for the same reason. No upper-case
    restriction; a lower-case hook is just as silent.
    """
    hooks = set(re.findall(r"^[ \t]*#[ \t]*ifdef[ \t]+([A-Za-z_]\w*)", src, re.M))
    return {h for h in hooks
            if h not in own and h not in included_names(src) and not GUARD_RE.match(h)}


def undeclared_calls(src, known):
    """Macros LK.c/LK.h invoke that nothing declares.

    No name list bounds this search, so the shape heuristic stays narrow:
    ALL-CAPS, call syntax, with the template's declarations and LK.c/LK.h's own
    #defines subtracted. That subtraction is what keeps NULL (LK.h:23-25) out
    of the report. No prefix skip-list, so an `LK_` hook is caught, not hidden.
    """
    out = set()
    for m in re.finditer(r"(?<![A-Za-z0-9_])([A-Z][A-Z0-9_]{2,})[ \t]*\(", src):
        n = m.group(1)
        if n not in known and not GUARD_RE.match(n):
            out.add(n)
    return out


def find_our_port(explicit=None):
    if explicit:
        if not os.path.isfile(explicit):
            sys.exit("--port %s does not exist" % explicit)
        return explicit
    inc = os.path.join(FW, "include")
    if not os.path.isdir(inc):
        return None
    found = sorted(os.path.join(inc, n) for n in os.listdir(inc)
                   if re.match(r"LK_device.*\.h$", n))
    if len(found) > 1:
        warn("%d candidate LK_device headers in include/, checking %s"
             % (len(found), os.path.basename(found[0])),
             ", ".join(os.path.basename(f) for f in found))
    return found[0] if found else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--lk", default=DEFAULT_LK, help="upstream checkout")
    ap.add_argument("--pin", action="store_true",
                    help="record the current upstream commit and hashes as the pin")
    ap.add_argument("--port", default=None, metavar="FILE",
                    help="check this LK_device header instead of auto-discovering "
                         "include/LK_device*.h (for reviewing a candidate port "
                         "before it lands)")
    args = ap.parse_args()

    lk = os.path.abspath(args.lk)
    if not os.path.isdir(lk) or not os.listdir(lk):
        sys.exit("upstream/FlashGBX_LK_Firmware is empty.\n"
                 "This is a git submodule. A plain `git clone` does not fetch it:\n"
                 "    git submodule update --init --recursive\n"
                 "(or pass --lk DIR to point somewhere else)")
    for f in VENDORED:
        if not os.path.exists(os.path.join(lk, f)):
            sys.exit("%s is missing from %s" % (f, lk))

    try:
        commit = subprocess.run(["git", "-C", lk, "rev-parse", "HEAD"],
                                capture_output=True, text=True).stdout.strip()
    except Exception:
        commit = ""
    text = {f: open(os.path.join(lk, f), encoding="utf-8", errors="replace").read()
            for f in VENDORED}
    hashes = {f: sha(os.path.join(lk, f)) for f in VENDORED}
    version = re.search(r"#define\s+LK_FIRMWARE_VERSION\s+(\d+)", text["LK.h"])
    version = int(version.group(1)) if version else None

    if args.pin:
        os.makedirs(os.path.dirname(PIN_FILE), exist_ok=True)
        json.dump({"commit": commit, "firmware_version": version,
                   "sha256": hashes}, open(PIN_FILE, "w"), indent=1)
        print("pinned upstream %s (L%s) to %s"
              % (commit[:12] or "?", version, os.path.relpath(PIN_FILE, FW)))
        return 0

    print("upstream: %s  LK_FIRMWARE_VERSION = L%s" % (commit[:12] or "(no git)", version))

    print("\n1. is the vendored copy still pristine?")
    if not os.path.exists(PIN_FILE):
        warn("no pin recorded yet",
             "run with --pin once the vendored copy is the one you intend to build")
    else:
        pin = json.load(open(PIN_FILE))
        if pin.get("commit") and commit and pin["commit"] != commit:
            print("  upstream has MOVED: pinned %s, checkout %s"
                  % (pin["commit"][:12], commit[:12]))
            print("  -> this is an upgrade, not a drift. Read the diff below, then --pin.")
        for f in VENDORED:
            same = pin.get("sha256", {}).get(f) == hashes[f]
            if pin.get("commit") == commit:
                ck(same, "%s matches its pinned hash" % f,
                   "LOCALLY MODIFIED. LK.c/LK.h must stay read-only. Put the change "
                   "in our LK_device header, in the transport, or upstream. If it is a "
                   "genuine upstream bug, keep it as a documented patch, not an inline edit.")
            elif not same:
                print("  [diff] %s changed upstream" % f)
        if pin.get("firmware_version") not in (None, version):
            print("  [!!!!] LK_FIRMWARE_VERSION moved L%s -> L%s"
                  % (pin["firmware_version"], version))
            print("         The WIRE PROTOCOL changes with this number. FlashGBX gates")
            print("         behaviour on it (PING gained a challenge byte at 15), so a")
            print("         bump is a protocol review, not a recompile.")

    print("\n2. what does upstream require of a port?")
    live, commented = template_macros(text["LK_device.h"])
    declared = live | commented
    src = strip_code(text["LK.c"] + text["LK.h"])
    own = define_names(text["LK.c"] + text["LK.h"])

    used = used_macros(src, declared)
    direct = sorted(m for m in used if m in live)
    bodies = macro_bodies(text["LK_device.h"])
    required = sorted(close_over_bodies(direct, bodies, live))
    indirect = sorted(set(required) - set(direct))

    hooks = ifdef_hooks(src, own)
    invoked = invoked_names(src)
    undeclared = sorted(undeclared_calls(src, declared | own))

    unused = sorted(m for m in live
                    if m not in required and not GUARD_RE.match(m))

    print("  LK_device.h declares %d macros; LK.c/LK.h reference %d of them"
          % (len(live), len(direct)))
    if indirect:
        print("  +%d reached only through another template macro, so still required: %s"
              % (len(indirect), ", ".join(indirect)))
        for m in indirect:
            via = sorted(k for k in required
                         if word(m).search(strip_code(bodies.get(k, ""))))
            print("      %-24s via %s" % (m, ", ".join(via)))
    print("  => %d required in total" % len(required))
    print("  %d declared but unused by LK.c/LK.h (pin aliases and the like): %s"
          % (len(unused), ", ".join(unused)))

    if commented:
        print("  %d are declared only as COMMENTS in the template: %s"
              % (len(commented), ", ".join(sorted(commented))))
    if undeclared:
        warn("%d macro(s) are called by LK.c but never declared in the template"
             % len(undeclared),
             ", ".join(undeclared) + "\n         These are the silent ones. See the "
             "header of this file. Decide for each\n         whether our board needs it, "
             "and record the decision either way.")

    optional = sorted(h for h in hooks if h in used or h in invoked)
    orphan = sorted(h for h in hooks if h not in optional)
    if optional:
        print("\n  optional #ifdef hooks (define them or silently skip them):")
        for h in optional:
            where = [str(i + 1) for i, l in enumerate(text["LK.c"].split("\n"))
                     if re.search(r"#[ \t]*ifdef[ \t]+%s\b" % re.escape(h), l)]
            print("    %-26s LK.c:%s" % (h, ",".join(where) or "?"))
    if orphan:
        print("  %d #ifdef name(s) guard no use of themselves: %s"
              % (len(orphan), ", ".join(orphan)))

    print("\n3. does our port implement them?")
    ours = find_our_port(args.port)
    if ours is None:
        print("  [SKIP] no include/LK_device*.h yet. The port has not started.")
        print("         When it does, this section becomes the guard that stops an")
        print("         upstream macro from being silently unimplemented.")
        print("         (--port FILE checks a candidate header before it lands.)")
        print("\n  a port will have to implement %d required macros, and decide"
              % len(required))
        print("  explicitly about %d optional hook(s)." % len(optional))
    else:
        print("  checking %s" % ours)
        text_ours = open(ours, encoding="utf-8", errors="replace").read()
        ours_live, _ = template_macros(text_ours)
        missing = sorted(m for m in required if m not in ours_live)
        ck(not missing, "every required macro (%d) is implemented by %s"
           % (len(required), os.path.basename(ours)), ", ".join(missing))
        skipped = sorted(h for h in optional if h not in ours_live)
        if skipped:
            warn("%d optional hook(s) not defined: upstream will skip them silently"
                 % len(skipped), ", ".join(skipped))
        extra = sorted(m for m in ours_live
                       if m not in live and m not in commented and m not in hooks
                       and not m.startswith(("_", "LK_DEVICE", "HARDWARE")))
        if extra:
            print("  %d macro(s) are ours alone (fine, but they are not upstream's "
                  "contract):" % len(extra))
            print("    " + ", ".join(extra[:12]))

    print("\n%s: %d passed, %d failed, %d warning(s)"
          % (os.path.basename(__file__), _pass, _fail, _warn))
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
