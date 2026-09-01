"""Read soak that can tell a bad dump from a device that stopped answering.

The earlier version could not. A wedged device fails a read in 0.18 s, and
counting that as "wrong data" turned one wedge into 61 corrupt reads. Every
read here is preceded by a health check, and a wedge is resynchronised and
counted separately, because the two have completely different meanings for a
release: one is data corruption, the other is a recoverable protocol stall.
"""
import subprocess, sys, os, time, hashlib
import serial, serial.tools.list_ports

SP = "/private/tmp/claude-501/-Users-damon-Documents-Claude-Code-Projects-GBFlash/a3a03a68-ed94-490a-a270-0bc0d1b64f6d/scratchpad"
PKG = SP + "/gbflash-bench"; FG = PKG + "/flashgbx"; out = PKG + "/results/_s4.bin"
GOOD = "e56abb530f3fe57e21666eae51ed2cea"

def port():
    for p in serial.tools.list_ports.comports():
        if (p.vid, p.pid) in ((0x1A86, 0x7523), (0x1209, 0x0008)):
            return p.device
    return None

def alive(resync=True):
    d = port()
    if d is None:
        return False
    try:
        s = serial.Serial(d, 2000000, timeout=1.0)
    except Exception:
        return False
    try:
        time.sleep(0.15); s.reset_input_buffer()
        s.write(bytes([0xA1])); s.flush()
        if s.read(1)[:1] not in (b"", b"\x00"):
            return True
        if not resync:
            return False
        for _ in range(3):
            s.write(b"\x00" * 2048); s.flush(); time.sleep(0.3)
            while s.read(65536):
                pass
            s.reset_input_buffer(); s.write(bytes([0xA1])); s.flush()
            if s.read(1)[:1] not in (b"", b"\x00"):
                return True
        return False
    finally:
        s.close()

N = int(sys.argv[1]) if len(sys.argv) > 1 else 100
clean = corrupt = wedge = unrecovered = 0
times = []
for i in range(N):
    if not alive():
        wedge += 1
        if not alive():
            unrecovered += 1
            print("  read %d: device would not resynchronise" % (i + 1)); break
        print("  read %d: device had stalled, resynchronised" % (i + 1))
    t0 = time.time()
    subprocess.run([sys.executable, "run.py", "--cli", "--mode", "dmg", "--action",
                    "backup-rom", out, "--overwrite", "--ignore-bad-header"],
                   cwd=FG, capture_output=True, stdin=subprocess.DEVNULL)
    dt = time.time() - t0; times.append(dt)
    if not os.path.exists(out):
        if alive(resync=False):
            corrupt += 1; print("  read %d: no dump, device still healthy (%.2f s)" % (i + 1, dt))
        else:
            wedge += 1; print("  read %d: no dump, device stalled (%.2f s)" % (i + 1, dt))
        continue
    d = open(out, "rb").read(); os.remove(out)
    if hashlib.md5(d).hexdigest() != GOOD or len(d) != 2097152:
        corrupt += 1
        print("  read %d: WRONG DATA %.2f s %d bytes md5 %s"
              % (i + 1, dt, len(d), hashlib.md5(d).hexdigest()[:8]))
    else:
        clean += 1
med = sorted(times)[len(times)//2] if times else 0
print("\n  %d reads: %d clean, %d wrong data, %d stalls (%d unrecovered)"
      % (len(times), clean, corrupt, wedge, unrecovered))
print("  median %.2f s  max %.2f s" % (med, max(times) if times else 0))
