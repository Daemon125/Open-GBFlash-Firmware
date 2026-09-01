#!/usr/bin/env python3
"""Fail the build when a comment in the sources asserts something untrue.

Four hard checks over src/, include/ and the Makefile, then one advisory report
over results/STATUS.md.

results/STATUS.md is an append-only dated journal. An entry saying "not gated
yet" was true the day it was written, so flagging it is wrong. The defect is
reading it as current, which has happened four times. The advisory report prints,
for each claim line, the newest entry naming the same token, so a reader who
greps a claim lands on its supersession instead of stopping there.

A comment in a source file has no such excuse. It describes the code beside it
and is either true now or a defect. Those are failures.

Every failure can be waived in EXEMPT below, and a waiver needs a reason string.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
STATUS = os.path.join(ROOT, "results", "STATUS.md")
FLASHGBX = os.path.abspath(os.path.join(ROOT, "..", "..", "FlashGBX-stock"))

# Sources whose comments must be true today.
SRC_DIRS = ("src", "include")
SRC_EXT = (".c", ".h", ".S")

# (file, needle) -> why it is allowed to stay. Keep this list short.
EXEMPT = {
    ("Makefile", "NOT GATED ON HARDWARE"):
        "two AUDIO-WE knobs, no cartridge with that profile exists here",
}

_pass = _fail = 0


def ck(cond, what, detail=""):
    global _pass, _fail
    if cond:
        _pass += 1
    else:
        _fail += 1
        print("  [FAIL] %s" % what)
        if detail:
            for line in detail.rstrip().split("\n"):
                print("         %s" % line)
    return cond


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def source_files():
    out = []
    for d in SRC_DIRS:
        full = os.path.join(ROOT, d)
        if not os.path.isdir(full):
            continue
        for name in sorted(os.listdir(full)):
            if name.endswith(SRC_EXT) and not name.endswith(".orig"):
                out.append(os.path.join(full, name))
    mk = os.path.join(ROOT, "Makefile")
    if os.path.exists(mk):
        out.append(mk)
    return out


def rel(path):
    return os.path.relpath(path, ROOT)


def exempt(path, needle):
    for (f, n), _reason in EXEMPT.items():
        if rel(path).endswith(f) and n in needle:
            return True
    return False


# ---------------------------------------------------------------- check 1

CITE = re.compile(r"\b([A-Za-z_][\w.\-]*\.(?:c|h|py|S)|Makefile):(\d+)\b")

# Citations naming a file this tree does not own. LK: is a label for the
# upstream FlashGBX_LK_Firmware listing, not a path.
FOREIGN = ("LK_Device.py", "Mapper.py", "Flashcart.py", "FlashGBX",
           "RomFileDMG.py", "RomFileAGB.py", "FlashGBX_CLI.py")


def locate(name):
    for base in (ROOT, FLASHGBX, os.path.join(FLASHGBX, "FlashGBX")):
        p = os.path.join(base, name)
        if os.path.exists(p):
            return p
    for d in SRC_DIRS + ("host", "tools", "ld", "patches"):
        p = os.path.join(ROOT, d, name)
        if os.path.exists(p):
            return p
    return None


def check_citations():
    print("\n1. every file:line citation in a source comment resolves")
    bad = []
    skipped = []
    for path in source_files():
        text = read(path)
        if text is None:
            continue
        for n, line in enumerate(text.split("\n"), 1):
            for m in CITE.finditer(line):
                name, num = m.group(1), int(m.group(2))
                target = locate(name)
                if target is None:
                    # A citation into FlashGBX can only be checked where a
                    # FlashGBX checkout sits beside this one. It does not in a
                    # fresh clone, and failing there would mean the repository
                    # cannot pass its own suite without an unrelated project
                    # downloaded next to it. Counted and reported instead.
                    if any(f in name for f in FOREIGN):
                        skipped.append("%s:%d cites %s" % (rel(path), n, name))
                    continue
                body = read(target)
                if body is None:
                    continue
                count = body.count("\n") + 1
                if num > count:
                    bad.append("%s:%d cites %s:%d, but that file ends at %d"
                               % (rel(path), n, name, num, count))
    if skipped:
        print("     %d citation(s) into FlashGBX not checked: no checkout beside"
              " this one" % len(skipped))
    ck(not bad, "no citation points past the end of its file",
       "\n".join(bad[:12]))


# ---------------------------------------------------------------- check 2

GATE_MARK = re.compile(r"\[NOT GATED\]|NOT GATED ON HARDWARE")
IDENT = re.compile(r"\b(fw_[a-z0-9_]+|FW_[A-Z0-9_]+|0x[0-9A-Fa-f]{2}\b)")


def status_sections():
    """[(date, start_line, text)] for each dated heading, in file order."""
    text = read(STATUS)
    if text is None:
        return []
    lines = text.split("\n")
    heads = []
    for i, line in enumerate(lines, 1):
        m = re.match(r"^#{1,2}\s+(20\d\d-\d\d-\d\d)", line)
        if m:
            heads.append((m.group(1), i))
    out = []
    for k, (date, start) in enumerate(heads):
        end = heads[k + 1][1] - 1 if k + 1 < len(heads) else len(lines)
        out.append((date, start, "\n".join(lines[start - 1:end])))
    return out


def check_gate_markers():
    print("\n2. no source marks a path ungated that STATUS.md later gated")
    secs = status_sections()
    bad = []
    for path in source_files():
        text = read(path)
        if text is None:
            continue
        for n, line in enumerate(text.split("\n"), 1):
            if not GATE_MARK.search(line) or exempt(path, line):
                continue
            names = [i for i in IDENT.findall(line) if i.startswith("fw_")]
            if not names:
                # the marker may sit under a knob block; look back a little
                back = "\n".join(text.split("\n")[max(0, n - 12):n])
                names = re.findall(r"^(FW_[A-Z0-9_]+)\s*\?=", back, re.M)
            for sym in names:
                for date, start, body in secs:
                    if sym in body and re.search(r"\[GATED\]|GATED ON HARDWARE"
                                                 r"|byte-exact|BYTE-EXACT", body):
                        bad.append("%s:%d marks %s ungated, but STATUS.md:%d "
                                   "(%s) gates it" % (rel(path), n, sym,
                                                      start, date))
                        break
    ck(not bad, "no stale ungated marker", "\n".join(bad[:12]))


# ---------------------------------------------------------------- check 3

# The subject must sit on the same line and BEFORE the phrase, so "the guards
# are in fw_a() and fw_b()" after "never reads it" does not match, and neither
# does "unused slot" beside an unrelated call. C's __attribute__((unused)) and
# (void) casts are skipped outright.
UNUSED = re.compile(
    r"\b((?:fw_|dmg_|bl_)[a-z0-9_]+|st->[a-z0-9_]+)\b"
    r"(?:\(\))?[^.\n]{0,40}?"
    r"\b(?:is |are )?(?:never (?:used|read|written|called)|"
    r"has no (?:caller|writer|consumer)|stored and never [a-z]+|"
    r"is dead code|is unused)", re.I)

UNUSED_SKIP = re.compile(r"__attribute__|\(void\)\s*[a-z_]|unused slot")


def tree_uses(sym, skip):
    """Count references to sym outside its own declaration file."""
    hits = []
    for d in SRC_DIRS + ("host",):
        full = os.path.join(ROOT, d)
        if not os.path.isdir(full):
            continue
        for name in sorted(os.listdir(full)):
            p = os.path.join(full, name)
            if not name.endswith((".c", ".h", ".py")) or name.endswith(".orig"):
                continue
            if os.path.abspath(p) == os.path.abspath(skip):
                continue
            body = read(p)
            if body is None:
                continue
            for n, line in enumerate(body.split("\n"), 1):
                stripped = line.strip()
                if stripped.startswith(("*", "/*", "//", "#")):
                    continue
                if re.search(r"\b%s\b" % re.escape(sym), line):
                    hits.append("%s:%d" % (rel(p), n))
    return hits


def check_unused_claims():
    print("\n3. no source comment calls a symbol unused while it has callers")
    bad = []
    for path in source_files():
        text = read(path)
        if text is None:
            continue
        for n, line in enumerate(text.split("\n"), 1):
            if exempt(path, line) or UNUSED_SKIP.search(line):
                continue
            m = UNUSED.search(line)
            if not m:
                continue
            sym = m.group(1)
            if sym.startswith("st->"):
                sym = sym[4:]
            uses = tree_uses(sym, path)
            if len(uses) >= 2:
                bad.append("%s:%d calls %s unused; it is referenced at %s"
                           % (rel(path), n, sym, ", ".join(uses[:4])))
    ck(not bad, "no false unused claim", "\n".join(bad[:12]))


# ---------------------------------------------------------------- check 4

DEFAULT_CLAIM = re.compile(
    r"(FW_[A-Z0-9_]+|BL_[A-Z0-9_]+)[^.\n]{0,60}?"
    r"(defaults? (?:to )?(off|on|0|1)|ships (0|1)|is (0|1) by default)", re.I)


def makefile_defaults():
    text = read(os.path.join(ROOT, "Makefile")) or ""
    out = {}
    for m in re.finditer(r"^([A-Z][A-Z0-9_]*)\s*\?=\s*(\S+)", text, re.M):
        out[m.group(1)] = m.group(2)
    return out


def check_knob_defaults():
    print("\n4. no source comment states a knob default the Makefile contradicts")
    defs = makefile_defaults()
    bad = []
    for path in source_files():
        text = read(path)
        if text is None:
            continue
        for n, line in enumerate(text.split("\n"), 1):
            m = DEFAULT_CLAIM.search(line)
            if not m or exempt(path, line):
                continue
            knob = m.group(1)
            if knob not in defs:
                continue
            claimed = (m.group(3) or m.group(4) or m.group(5) or "").lower()
            want = {"off": "0", "on": "1"}.get(claimed, claimed)
            actual = defs[knob]
            if want and actual.isdigit() and want != actual:
                bad.append("%s:%d says %s is %s; Makefile ships %s"
                           % (rel(path), n, knob, claimed, actual))
    ck(not bad, "no contradicted knob default", "\n".join(bad[:12]))


# ---------------------------------------------------------------- advisory

CLAIM_WORDS = re.compile(
    r"(not gated|never met|transcription.only|needs a .{0,24}cartridge|"
    r"on order|not implemented|never used|is not fixed|still fails|"
    r"unverified|no such cartridge|nobody has)", re.I)

TOKEN = re.compile(r"\b(0x[A-F0-9]{2}|FW_[A-Z0-9_]+|fw_[a-z0-9_]+)\b")


def report_supersession():
    """Print, for each claim line, the newest entry naming the same token."""
    print("\n5. STATUS.md claim lines that a later entry may supersede")
    secs = status_sections()
    if not secs:
        print("     STATUS.md not found, skipped")
        return
    text = read(STATUS)
    lines = text.split("\n")

    def section_of(lineno):
        cur = None
        for date, start, _ in secs:
            if start <= lineno:
                cur = (date, start)
            else:
                break
        return cur

    latest = {}
    for date, start, body in secs:
        for tok in set(TOKEN.findall(body)):
            prev = latest.get(tok)
            if prev is None or (date, start) > (prev[0], prev[1]):
                latest[tok] = (date, start)

    rows = []
    for n, line in enumerate(lines, 1):
        if not CLAIM_WORDS.search(line):
            continue
        sec = section_of(n)
        if sec is None:
            continue
        toks = [t for t in TOKEN.findall(line) if t in latest]
        for tok in toks:
            ldate, lstart = latest[tok]
            if (ldate, lstart) > (sec[0], sec[1]):
                rows.append((n, sec[0], tok, lstart, ldate))
    if not rows:
        print("     none")
        return
    seen = set()
    shown = 0
    for n, sdate, tok, lstart, ldate in rows:
        key = (n, tok)
        if key in seen:
            continue
        seen.add(key)
        shown += 1
        if shown <= 25:
            print("     :%-5d (%s) claims about %-24s newest is :%-5d (%s)"
                  % (n, sdate, tok, lstart, ldate))
    print("     %d claim/token pairs; advisory only, this check never fails"
          % len(seen))


def main():
    print("test_stale_claims: source comments must be true now; STATUS.md is "
          "history")
    check_citations()
    check_gate_markers()
    check_unused_claims()
    check_knob_defaults()
    report_supersession()
    print("\ntest_stale_claims: %d passed, %d failed" % (_pass, _fail))
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
