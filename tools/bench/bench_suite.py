#!/usr/bin/env python3
"""Compare stock GBFlash firmware against Open-GBFlash, on this machine.

Flashes each firmware in turn and times the same work through FlashGBX, then
writes results/benchmark-<host>.txt. Run it, answer the cartridge prompts, and
send the txt back.

Stock is measured twice on reads: once through unmodified FlashGBX and once
through the patched FlashGBX this project ships. The patched host negotiates up
to 0x8000 only for Open-GBFlash and pins a stock device at 0x1000, upstream's
value, so the two stock rows should agree. They are the check that the host is
not contributing to the firmware comparison.
"""

import argparse
import hashlib
import re
import os
import platform
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FGBX = os.path.join(HERE, "flashgbx")
RUNPY = os.path.join(FGBX, "run.py")
HOSTPY = os.path.join(FGBX, "FlashGBX", "hw_GBFlash.py")
HOSTS = os.path.join(HERE, "hosts")
FW = os.path.join(HERE, "fw")
RESULTS = os.path.join(HERE, "results")
ROMS = os.path.join(HERE, "roms")

STOCK = os.path.join(FW, "stock_L15.bin")
OURS = os.path.join(FW, "gbflash_open.bin")

AGB_METHODS = [("Single", "0"), ("Stream", "2")]

rows = []
notes = []


class CartAborted(Exception):
    """This cartridge cannot yield more numbers, but other cartridges can."""


# Auto-detection picks by flash id, and several profiles share one. On one
# ChisFlash 2 MiB board it chose "GBFlash MBCX (32 MiB)", which erased sector 0
# and then failed at 0x4000, leaving the cartridge unreadable. Tried in order
# when a write fails and no profile was given.
#
# FlashGBX does not ship all of these. A name it does not know is reported as a
# failed write with the profiles it does know listed, so an unknown name costs a
# retry rather than confusing the result. Pass --dmg-flashcart to name your own.
PROFILE_FALLBACKS = {
    "dmg": ["ChisFlash MBC3 (2 MiB)", "ChisFlash MBC5 (2 MiB)",
            "ChisFlash MBC5 single-write", "ChisFlash-MBC5 PLUS v1.21"],
    "agb": [],
}


def loadavg():
    try:
        return os.getloadavg()[0]
    except (AttributeError, OSError):
        return None


def cores():
    return os.cpu_count() or 1


# What FlashGBX needs, beyond the standard library. Its pyproject lists these
# and the CLI path imports serial, dateutil, PIL and packaging directly.
DEPS = [("serial", "pyserial"), ("dateutil", "python-dateutil"),
        ("PIL", "Pillow"), ("packaging", "packaging")]


def check_flashgbx_runs():
    """Import FlashGBX's CLI in a child interpreter before touching anything.

    A missing dependency does not announce itself: FlashGBX dies on import, the
    suite sees an empty output file, and every read is reported as "no data;
    check the cartridge". A Windows run flashed the firmware twice and produced
    six of those before anyone could see the real cause was a missing
    python-dateutil. This costs a second and names the problem instead.
    """
    probe = ("import sys; sys.path.insert(0, %r); "
             "from FlashGBX import FlashGBX_CLI" % FGBX)
    r = subprocess.run([sys.executable, "-c", probe], capture_output=True,
                       text=True, errors="replace")
    if r.returncode == 0:
        return
    missing = []
    for mod, pkg in DEPS:
        c = subprocess.run([sys.executable, "-c", "import " + mod],
                           capture_output=True)
        if c.returncode != 0:
            missing.append(pkg)
    say()
    say("  FlashGBX cannot start on this machine.")
    say()
    if missing:
        say("  Missing Python packages: %s" % ", ".join(missing))
        say("  Install them with:")
        say("      %s -m pip install %s" % (os.path.basename(sys.executable),
                                            " ".join(missing)))
    else:
        say("  Its import failed for a reason other than a missing package:")
        for line in (r.stdout + r.stderr).strip().splitlines()[-6:]:
            say("      %s" % line.rstrip())
    say()
    sys.exit("stopped before touching the device")


def spin_score(n=6):
    """Best single-core throughput this machine will currently give us.

    Reported as (seconds, wall/cpu). The two catch different things and neither
    catches both:

      wall/cpu near 1.0 means we are getting a whole core, so a slow result
      is the core being slow, not us being preempted. On Apple Silicon that is
      usually an efficiency core or a low clock, and it swung 3x inside a
      minute on this host.

      seconds is the absolute figure. Compared against the best this run has
      seen, it says whether the machine was as fast later as it was earlier.

    Best of n, because contention only ever adds time.
    """
    best = None
    for _ in range(n):
        w0 = time.time()
        c0 = time.process_time()
        x = 0
        for i in range(1500000):
            x += i * i
        w = time.time() - w0
        c = time.process_time() - c0
        if best is None or w < best[0]:
            best = (w, w / c if c else 0.0)
    return best


def check_idle():
    """Warn, never block.

    An earlier version refused to start above a load-average threshold and
    threw away a good run on a machine that was fine. Load average counts
    threads wanting a core; it cannot see whether the core we get is fast. The
    defence that actually works is elsewhere: every read is repeated, dumps
    that disagree are rejected, and the fastest surviving sample wins, so a
    contended stretch loses to a quiet one instead of poisoning the result.
    """
    la = loadavg()
    if la is not None and la > cores():
        say("  Note: load average %.1f on %d cores. Timings will be taken as"
            % (la, cores()))
        say("  the best of several reads, so this costs time rather than")
        say("  accuracy, but closing other work will make it quicker.")
        say()


def ask(prompt):
    """Prompts must not turn a piped or unattended run into a traceback."""
    try:
        return input(prompt)
    except EOFError:
        print("")
        return ""


def say(s=""):
    print(s)
    sys.stdout.flush()


def known_profiles(mode):
    """Names FlashGBX will accept for --flashcart-type, from its own config.

    Read with a regex, not json.load: the profiles use hex literals like
    0x200000, which is not valid JSON and fails on 100 of the 101 DMG files.
    FlashGBX has its own lenient parser; only the names are needed here.
    """
    cfg = os.path.join(FGBX, "FlashGBX", "config")
    names = []
    try:
        listing = sorted(os.listdir(cfg))
    except OSError:
        return names
    for fn in listing:
        if not fn.startswith("fc_%s_" % mode.upper()) or not fn.endswith(".txt"):
            continue
        try:
            with open(os.path.join(cfg, fn), "r", encoding="utf-8",
                      errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        m = re.search(r'"names"\s*:\s*\[(.*?)\]', text, re.S)
        if m:
            names.extend(re.findall(r'"([^"]+)"', m.group(1)))
    return names


def drop_bootlogo():
    """Remove FlashGBX's cached boot logo.

    Housekeeping only. It does NOT stop the prompt: a single flash-rom run
    reads the cartridge header first, and a cartridge with a valid logo
    recreates this file at FlashGBX_CLI.py:734, before the check at :1184 that
    the prompt hangs off. Deleting it beforehand is therefore useless, which is
    how the first attempt at this failed. Writes answer the prompt instead.
    """
    cfg = os.path.join(FGBX, "FlashGBX", "config")
    for n in ("bootlogo_agb.bin", "bootlogo_dmg.bin"):
        try:
            os.remove(os.path.join(cfg, n))
        except OSError:
            pass


def set_host(which):
    shutil.copyfile(os.path.join(HOSTS, "hw_GBFlash_%s.py" % which), HOSTPY)
    pyc = os.path.join(FGBX, "FlashGBX", "__pycache__")
    shutil.rmtree(pyc, ignore_errors=True)


def flash(image, label):
    say("  flashing %s ..." % label)
    r = subprocess.run([sys.executable, os.path.join(HERE, "flash_fw.py"), image],
                       capture_output=True, text=True)
    sys.stdout.write("".join("    " + l + "\n"
                             for l in (r.stdout + r.stderr).strip().splitlines()
                             if l.strip()))
    if r.returncode != 0:
        notes.append("flashing %s failed; last output:\n%s"
                     % (label, tail(r.stdout + r.stderr)))
        raise RuntimeError(
            "could not flash %s; stopping so nothing below is misread. If the "
            "device stopped answering, hold U22 while plugging it in and rerun."
            % label)
    time.sleep(2.0)


def fgbx(args, env_extra=None, feed=None):
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    if env_extra:
        env.update(env_extra)
    t0 = time.time()
    kw = {"input": feed} if feed is not None else {"stdin": subprocess.DEVNULL}
    r = subprocess.run([sys.executable, RUNPY, "--cli"] + args, cwd=FGBX,
                       capture_output=True, text=True, env=env,
                       errors="replace", **kw)
    return time.time() - t0, (r.stdout or "") + (r.stderr or ""), r.returncode


def read_rom(mode, method=None):
    out = os.path.join(RESULTS, "_dump.bin")
    # Always, not just for DMG. A test ROM has no Nintendo logo, so once one
    # has been written the cartridge header reads as "invalid data" and
    # backup-rom returns 1 without dumping anything. The dumps are checked
    # against each other by md5 here, which is a stronger check than the one
    # this switch turns off.
    args = ["--mode", mode, "--action", "backup-rom", out, "--overwrite",
            "--ignore-bad-header"]
    env = {"GBFLASH_AGB_READ_METHOD": method} if method else None
    dt, txt, _ = fgbx(args, env)
    if not os.path.exists(out) or os.path.getsize(out) == 0:
        return None, None, None, txt
    n = os.path.getsize(out)
    h = hashlib.md5()
    with open(out, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    os.remove(out)
    return dt, n, h.hexdigest(), txt


def write_rom(mode, rom, flashcart=None):
    """Time one flash-rom. Returns (seconds or None, output, verified).

    Success is the exit code, not a message. FlashGBX sets RETVAL=1 on a
    verification failure and returns it, and it has three different endings
    that string matching gets wrong in both directions:

      "written and verified successfully"   wrote and verified
      "ROM writing complete!"               wrote, verification did not run
      "written completely, but verification of written data failed"   FAILED

    Matching the first alone rejects the second, which is a real success.
    Matching on "complete" accepts the third, which is a corrupted write. Both
    mistakes were made here in turn.

    `verified` is returned separately because an unverified write skips a full
    read back of the cartridge. Comparing a verified write against an
    unverified one compares different amounts of work.
    """
    drop_bootlogo()
    args = ["--mode", mode, "--action", "flash-rom", rom, "--overwrite",
            "--ignore-bad-header"]
    if flashcart:
        args += ["--flashcart-type", flashcart]
    # "n" to "Fix the boot logo before continuing?", so the ROM is written
    # exactly as generated. The test ROMs have no Nintendo logo and cannot: it
    # is not ours to ship. Answering "y" would rewrite the first 0xA0 bytes and
    # the file on disk would stop matching the cartridge.
    dt, txt, rc = fgbx(args, feed="n\n" * 64)
    low = txt.lower()
    ok = (rc == 0
          and "verification of written data failed" not in low
          and ("written and verified successfully" in low
               or "rom writing complete" in low))
    verified = "written and verified successfully" in low
    return (dt if ok else None), txt, verified


def tail(txt, lines=14):
    """Last few meaningful lines. FlashGBX emits a progress bar per chunk, and
    those alone overflow any fixed-size tail, hiding the error that matters."""
    keep = [l.rstrip() for l in txt.splitlines()
            if l.strip() and "ETA" not in l and "\u2588" not in l
            and "\u258c" not in l and "\u2591" not in l]
    return "\n".join(keep[-lines:])


def add(cart, fwname, host, op, seconds, nbytes, extra=""):
    kib = (nbytes / seconds / 1024.0) if (seconds and nbytes) else 0.0
    rows.append((cart, fwname, host, op, seconds, kib, extra))
    if seconds:
        say("    %-34s %8.2f s  %6.0f KiB/s  %s" % (op, seconds, kib, extra))
    else:
        say("    %-34s   FAILED  %s" % (op, extra))


def phase_reads(cart, mode, fwname, host, methods, reads):
    """Repeat each read, discard the ones that disagree, keep the fastest.

    A short dump is the dangerous case. FlashGBX can stop early and still exit
    leaving a file behind, and a run that stopped early is FASTER, so a
    best-of-N that only checks "did a file appear" will reliably select the
    truncated one. That happened here: a 31 MiB dump of a 64 MiB cartridge was
    accepted at 163 s against a good run's 285 s, and picked as the best.

    So a read counts only if its size and md5 match the majority of the reads
    of the same cartridge. Timing a dump that disagrees byte for byte with the
    others is timing a failure.
    """
    got = {}
    for label, method in methods:
        runs = []
        for i in range(reads):
            dt, n, md5, txt = read_rom(mode, method)
            op = ("ROM read" + (" (%s)" % label if label else "")
                  + (" run %d" % (i + 1) if reads > 1 else ""))
            if dt is None:
                add(cart, fwname, host, op, None, None,
                    "no data; check the cartridge")
                notes.append("%s %s/%s %s produced no dump"
                             % (cart, fwname, host, op))
                continue
            runs.append((dt, n, md5, op))

        if not runs:
            continue

        # Consensus on (size, md5). Ties break towards the larger dump: a
        # truncation is always short, never long.
        tally = {}
        for dt, n, md5, op in runs:
            tally[(n, md5)] = tally.get((n, md5), 0) + 1
        best_key = sorted(tally, key=lambda k: (-tally[k], -k[0]))[0]

        best = None
        for dt, n, md5, op in runs:
            agrees = (n, md5) == best_key
            add(cart, fwname, host, op, dt, n,
                ("md5 " + md5) if agrees
                else "REJECTED, %d bytes md5 %s disagrees" % (n, md5))
            if not agrees:
                notes.append(
                    "%s %s/%s %s disagreed with the other reads: %d bytes, md5 "
                    "%s, against %d bytes md5 %s. Not counted. A short or "
                    "differing dump means the read failed, not that it was fast."
                    % (cart, fwname, host, op, n, md5, best_key[0], best_key[1]))
                continue
            got[label] = md5
            if best is None or dt < best[0]:
                best = (dt, n)

        agreeing = sum(tally[k] for k in tally if k == best_key)
        if best and reads > 1:
            add(cart, fwname, host,
                "ROM read" + (" (%s)" % label if label else "") + " BEST",
                best[0], best[1], "fastest of %d agreeing" % agreeing)
        if best and reads > 1 and agreeing < 2:
            notes.append(
                "%s %s/%s rests on ONE good read out of %d. The others failed "
                "or disagreed, which points at the cartridge or the connection "
                "rather than at the firmware. Reseat it and rerun before "
                "trusting this row."
                % (cart, fwname, host, reads))
    return got


def write_pair(cart, mode, fwname, roms, flashcart):
    """Write A untimed to normalise the cartridge, then time writing B.

    FlashGBX skips blocks that already match the cartridge, so a timed write is
    only meaningful if the starting contents are known and the same for both
    firmwares. Timing both A and B leaves the first measuring whatever happened
    to be on the cartridge beforehand.

    A failed write is not just a missing row. FlashGBX erases a sector before it
    writes, so a write that fails part way leaves the cartridge without a
    header, and every later read of it fails too. Those failures are the
    cartridge, not the firmware being measured, so this raises rather than
    letting the run collect them.
    """
    (_, prime), (_, timed) = roms

    def attempt(rom, label):
        dt, txt, ver = write_rom(mode, rom, flashcart)
        if dt is not None or flashcart:
            return dt, txt, ver, flashcart
        for prof in PROFILE_FALLBACKS.get(mode, []):
            say("      retrying %s with profile %r" % (label, prof))
            dt, txt, ver = write_rom(mode, rom, prof)
            if dt is not None:
                notes.append("%s %s: auto-detection chose a profile that could "
                             "not write. Used %r instead."
                             % (cart, fwname, prof))
                return dt, txt, ver, prof
        return None, txt, False, None

    say("    priming with %s (not timed) ..." % os.path.basename(prime))
    dt, txt, _, prof = attempt(prime, "prime")
    if dt is None:
        add(cart, fwname, "patched", "ROM write", None, None,
            "priming write failed")
        notes.append("%s %s priming write failed; last output:\n%s"
                     % (cart, fwname, tail(txt)))
        raise CartAborted(
            "%s: a write failed part way, so the cartridge has an erased "
            "sector and no usable header. Later reads of it would fail for "
            "that reason rather than because of the firmware. Reflash the "
            "cartridge before rerunning." % cart)

    dt, txt, verified, _ = attempt(timed, "timed write")
    add(cart, fwname, "patched", "ROM write", dt,
        os.path.getsize(timed),
        ("verified, " if verified else "NOT verified, ")
        + "wrote %s over %s%s" % (os.path.basename(timed),
                                  os.path.basename(prime),
                                  ", profile %r" % prof if prof else ""))
    if dt is None:
        notes.append("%s %s timed write failed; last output:\n%s"
                     % (cart, fwname, tail(txt)))
        raise CartAborted("%s: timed write failed, cartridge left dirty" % cart)
    if not verified:
        notes.append(
            "%s %s wrote without verifying. A verified write also reads the "
            "whole cartridge back, so this time is not comparable with a "
            "verified one." % (cart, fwname))


def run_cart(cart, mode, methods, do_writes, roms, flashcart, quick, reads,
             writes_only=False):
    """All reads first, then all writes.

    The order matters more than it looks. Reading stock, then writing with
    stock, then reading with this firmware means the two firmwares read
    different cartridge contents: the second one reads whatever the first one
    wrote. On the DMG cartridge that was 512 KiB against 2 MiB, a 4x difference
    in work presented as a throughput comparison. The md5 consensus caught it,
    which is the only reason it is not in the published table.

    So both firmwares read the same contents, then both write the same file
    over the same contents. It costs two extra firmware flashes.
    """
    if quick:
        methods = methods[-1:]
    say()
    say("=" * 72)
    say("  %s" % cart)
    say("=" * 72)

    md5s = {}
    if writes_only:
        methods = []

    flash(STOCK, "stock L15")

    # A cartridge holding a real game reports whatever size its header declares,
    # which on one DMG cartridge was 512 KiB of a 2 MiB part. Priming with the
    # test ROM makes every read cover the whole part.
    #
    # Done AFTER the first firmware flash, not before: run before it, this runs
    # on whatever firmware and host file happen to be sitting there, which is
    # not a state this script chose.
    #
    # A prime that fails costs the writes, not the reads. Reads only need both
    # firmwares to see the SAME contents, and nothing writes between them, so
    # they stay valid on whatever the cartridge already holds.
    if do_writes and roms and methods:
        set_host("patched")
        say("  priming the cartridge with %s so every read covers the whole"
            % os.path.basename(roms[0][1]))
        say("  cartridge (not timed) ...")
        dt, txt, _ = write_rom(mode, roms[0][1], flashcart)
        if dt is None and not flashcart:
            for prof in PROFILE_FALLBACKS.get(mode, []):
                say("      retrying with profile %r" % prof)
                dt, txt, _ = write_rom(mode, roms[0][1], prof)
                if dt is not None:
                    flashcart = prof
                    notes.append("%s: auto-detection chose a profile that "
                                 "could not write. Used %r instead."
                                 % (cart, prof))
                    break
        if dt is None:
            do_writes = False
            say()
            say("  COULD NOT WRITE TO THIS CARTRIDGE, so the write benchmark is")
            say("  skipped for it. The reads below still run and are still")
            say("  valid: nothing writes between them.")
            say()
            say("  The usual causes, in order of likelihood:")
            say("    - no cartridge inserted, or the wrong one for this prompt")
            say("    - a %s cartridge that is not a FLASH cartridge" % mode.upper())
            say("    - FlashGBX picked the wrong flashcart profile")
            say()
            profs = known_profiles(mode)
            tried = PROFILE_FALLBACKS.get(mode, [])
            if tried:
                say("  Already tried automatically: %s" % ", ".join(tried))
            if profs:
                say("  For the last one, name the profile yourself:")
                say("      RUN.bat --%s-flashcart \"<name>\"" % mode)
                shown = [n for n in profs if "chisflash" in n.lower()][:6]
                if shown:
                    say("  ChisFlash profiles in this copy of FlashGBX:")
                    for n in shown:
                        say("      %s" % n)
                say("  %d %s profiles are available in total; the full list is"
                    % (len(profs), mode.upper()))
                say("  in flashgbx/FlashGBX/config/fc_%s_*.txt" % mode.upper())
                say()
            say("  What FlashGBX said:")
            for line in tail(txt, 10).splitlines():
                say("      %s" % line)
            say()
            notes.append("%s: priming write failed, so writes were skipped for "
                         "this cartridge. Reads are unaffected. Last output:\n%s"
                         % (cart, tail(txt)))

    set_host("pristine")
    say("  stock L15  +  unmodified FlashGBX (read buffer 0x1000)")
    md5s["stock/pristine"] = phase_reads(cart, mode, "stock L15", "pristine",
                                         methods[-1:], reads)
    set_host("patched")
    say("  stock L15  +  patched FlashGBX (pins stock at 0x1000 too)")
    md5s["stock/patched"] = phase_reads(cart, mode, "stock L15", "patched",
                                        methods, reads)

    flash(OURS, "Open-GBFlash")
    set_host("patched")
    say("  Open-GBFlash  +  patched FlashGBX")
    md5s["ours/patched"] = phase_reads(cart, mode, "Open-GBFlash", "patched",
                                       methods, reads)

    seen = {}
    for who, d in md5s.items():
        for label, md5 in d.items():
            seen.setdefault(md5, []).append("%s %s" % (who, label))
    if len(seen) > 1:
        notes.append("%s: DUMPS DISAGREE, so a speed number here means nothing:\n%s"
                     % (cart, "\n".join("    %s  %s" % (m, ", ".join(v))
                                        for m, v in seen.items())))
        say("  !! dumps disagree between runs; see the notes in the report")
    elif seen:
        say("  all dumps identical: md5 %s" % list(seen)[0])

    # ---- writes, both firmwares, same file over the same contents ----
    if do_writes and roms:
        flash(STOCK, "stock L15")
        set_host("patched")
        say("  stock L15, write")
        write_pair(cart, mode, "stock L15", roms, flashcart)

        flash(OURS, "Open-GBFlash")
        set_host("patched")
        say("  Open-GBFlash, write")
        write_pair(cart, mode, "Open-GBFlash", roms, flashcart)


def report(path, elapsed, wrote, spin0, spin1, aborted=None):
    with open(path, "w") as f:
        w = f.write
        w("GBFlash firmware benchmark\n")
        w("=" * 72 + "\n\n")
        w("  host       %s %s (%s)\n" % (platform.system(), platform.release(),
                                         platform.machine()))
        w("  python     %s\n" % platform.python_version())
        w("  stock fw   %s\n" % md5file(STOCK))
        w("  open fw    %s\n" % md5file(OURS))
        w("  writes     %s\n" % ("yes" if wrote else "skipped"))
        la = loadavg()
        w("  load avg   %s on %d cores%s\n"
          % ("%.2f" % la if la is not None else "unknown", cores(),
             "   OVERSUBSCRIBED, timings understate Open-GBFlash"
             if (la is not None and la > cores()) else ""))
        w("  cpu probe  %.3f s before, %.3f s after (best single-core)%s\n"
          % (spin0[0], spin1[0],
             "   SLOWER LATER, the machine was not equally fast throughout"
             if spin0[0] and spin1[0] > spin0[0] * 1.25 else ""))
        w("  wall/cpu   %.2f before, %.2f after%s\n"
          % (spin0[1], spin1[1],
             "   PREEMPTED, other work was taking the core"
             if max(spin0[1], spin1[1]) > 1.25 else ""))
        w("  elapsed    %.1f min\n" % (elapsed / 60.0))
        if aborted:
            w("\n  *** INCOMPLETE: %s\n"
              "  *** The rows below are only what finished. Do not compare\n"
              "  *** across firmwares from a partial run.\n" % aborted)
        w("\n")
        w("  'pristine' is FlashGBX as shipped, read buffer pinned to 0x1000.\n")
        w("  'patched'  is this project's FlashGBX. It negotiates up to 0x8000\n")
        w("             for Open-GBFlash and pins a stock device at 0x1000.\n")
        w("  The two stock rows should agree: that is the check that the host is\n")
        w("  not contributing. Open-GBFlash needs the patched host to be seen.\n\n")
        w("%-26s %-13s %-9s %-24s %10s %11s\n"
          % ("cartridge", "firmware", "host", "operation", "seconds", "KiB/s"))
        w("-" * 98 + "\n")
        for cart, fwn, host, op, sec, kib, extra in rows:
            w("%-26s %-13s %-9s %-24s %10s %11s  %s\n"
              % (cart[:26], fwn, host, op,
                 ("%.2f" % sec) if sec else "FAILED",
                 ("%.0f" % kib) if sec else "-", extra))
        if notes:
            w("\n\nnotes\n" + "-" * 72 + "\n")
            for n in notes:
                w("  " + n.replace("\n", "\n  ") + "\n\n")
    say()
    say("  report written to %s" % path)


def md5file(p):
    h = hashlib.md5()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 16), b""):
            h.update(c)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--reads", type=int, default=3,
                    help="reads per configuration; the fastest is the result")
    ap.add_argument("--quick", action="store_true",
                    help="reads only, one method, no writes")
    ap.add_argument("--no-writes", action="store_true")
    ap.add_argument("--writes-only", action="store_true",
                    help="skip the reads; for retrying a failed write phase")
    ap.add_argument("--dmg-flashcart", help="--flashcart-type for DMG writes")
    ap.add_argument("--agb-flashcart", help="--flashcart-type for AGB writes")
    ap.add_argument("--skip", default="", help="comma separated: dmg,agb,m3d")
    args = ap.parse_args()

    if not os.path.exists(RUNPY):
        sys.exit("flashgbx/run.py is missing; unpack the whole folder")
    try:
        import serial  # noqa: F401
    except ImportError:
        sys.exit("pyserial is not installed. Run:  python -m pip install pyserial")

    os.makedirs(RESULTS, exist_ok=True)
    check_flashgbx_runs()
    drop_bootlogo()
    # A previous run's report left on disk reads as this run's result.
    for stale in os.listdir(RESULTS):
        if stale.startswith("benchmark-") and stale.endswith(".txt"):
            os.remove(os.path.join(RESULTS, stale))
    check_idle()
    skip = set(s.strip() for s in args.skip.split(",") if s.strip())

    say()
    say("  GBFlash firmware benchmark")
    say("  " + "-" * 60)
    say("  Flashes stock and Open-GBFlash in turn and times both.")
    say("  Takes roughly 15 minutes without writes, 40 with them.")
    say()
    say("  Leave the device plugged in and do not unplug it mid-run.")
    say("  If it ever stops responding: hold U22 while plugging in, then rerun.")
    say()

    if args.quick:
        args.reads = min(args.reads, 1)
    do_writes = not (args.quick or args.no_writes) or args.writes_only
    if do_writes:
        say("  WRITE BENCHMARKS ERASE THE DMG AND AGB CARTRIDGES YOU INSERT.")
        say("  Whatever is on them now is gone, saves included. The 3D Memory")
        say("  cartridge is read only and is never written.")
        say()
        if ask("  Include write benchmarks? Type YES to include: ").strip() != "YES":
            do_writes = False
            say("  skipping writes; reads only")
    if do_writes:
        say()
        say("  building the two test ROMs (this takes a moment) ...")
        os.makedirs(ROMS, exist_ok=True)
        r = subprocess.run([sys.executable, os.path.join(HERE, "mkroms.py"), ROMS],
                           capture_output=True, text=True)
        sys.stdout.write(r.stdout + r.stderr)
        if r.returncode != 0:
            sys.exit("could not build the test ROMs")

    plan = [
        ("dmg", "DMG flash cartridge", "dmg", [("", None)],
         [("A", os.path.join(ROMS, "dmg_a.gb")), ("B", os.path.join(ROMS, "dmg_b.gb"))],
         args.dmg_flashcart,
         "Insert your DMG (Game Boy) FLASH cartridge."),
        ("agb", "AGB flash cartridge", "agb", AGB_METHODS,
         [("A", os.path.join(ROMS, "agb_a.gba")), ("B", os.path.join(ROMS, "agb_b.gba"))],
         args.agb_flashcart,
         "Insert your AGB (Game Boy Advance) FLASH cartridge."),
        ("m3d", "3D Memory (GBA Video)", "agb", [("Stream", "2")], None, None,
         "Insert the 3D Memory cartridge (GBA Video, e.g. Shark Tale).\n"
         "  This one is read only. Nothing is written to it.\n"
         "  Press Enter to skip it if you do not have one."),
    ]

    spin0 = spin_score()
    say("  cpu probe: %.3f s single-core, wall/cpu %.2f" % spin0)
    t0 = time.time()
    wrote = False
    aborted = None
    try:
        for key, label, mode, methods, roms, flashcart, prompt in plan:
            if key in skip:
                continue
            say()
            say("  " + prompt)
            if ask("  Press Enter when it is seated, or s to skip: "
                   ).strip().lower() == "s":
                say("  skipped")
                continue
            cart_writes = do_writes and roms is not None
            wrote = wrote or cart_writes
            try:
                run_cart(label, mode, methods, cart_writes, roms, flashcart,
                         args.quick, args.reads, args.writes_only)
            except CartAborted as e:
                say()
                say("  SKIPPING REST OF THIS CARTRIDGE: %s" % e)
                notes.append(str(e))
    except (RuntimeError, KeyboardInterrupt) as e:
        aborted = str(e) or e.__class__.__name__
        say()
        say("  STOPPED: %s" % aborted)

    name = "benchmark-%s.txt" % platform.system().lower()
    report(os.path.join(RESULTS, name), time.time() - t0, wrote,
           spin0, spin_score(), aborted)
    say()
    say("  The device is left running Open-GBFlash.")
    say("  Send back:  results/%s" % name)
    say()
    ask("  Press Enter to close: ")


if __name__ == "__main__":
    main()
