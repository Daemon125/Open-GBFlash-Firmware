#!/usr/bin/env python3
"""The LK seam: routing, the state mirror, and the carried patches.

tools/lk_upstream_diff.py proves the vendored copy is pristine; it cannot see
the port's own defects.

Every check here is static: it reads the sources. No compiler, no device.
"""

import io
import os
import re
import subprocess
import sys
import shutil
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LKC = os.path.join(ROOT, "upstream", "FlashGBX_LK_Firmware", "LK.c")
LKH = os.path.join(ROOT, "upstream", "FlashGBX_LK_Firmware", "LK.h")
GLUE = os.path.join(ROOT, "src", "lk_glue.c")
GLUE_H = os.path.join(ROOT, "include", "lk_glue.h")
DEV_H = os.path.join(ROOT, "include", "LK_device_ch579.h")
PROTO_H = os.path.join(ROOT, "include", "proto.h")
PROTO_C = os.path.join(ROOT, "src", "proto.c")
MAIN_C = os.path.join(ROOT, "src", "main.c")
MAKEFILE = os.path.join(ROOT, "Makefile")

_pass = _fail = 0


def ck(cond, what, detail=""):
    global _pass, _fail
    if cond:
        _pass += 1
        print("  [ ok ] %s" % what)
    else:
        _fail += 1
        print("  [FAIL] %s%s" % (what, ("\n         " + detail) if detail else ""))
    return bool(cond)


def read(p):
    return io.open(p, encoding="utf-8", errors="replace").read()


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//.*", "", src)


def routed_opcodes(glue_src, proto_h):
    body = glue_src[glue_src.index("int fw_lk_routes("):]
    body = body[:body.index("\n}\n")]
    names = re.findall(r"case\s+(CMD_[A-Z0-9_]+)\s*:", body)
    out = {}
    for n in names:
        m = re.search(r"#define\s+%s\s+0x([0-9A-Fa-f]{2})u?" % n, proto_h)
        if not m:
            sys.exit("test_lk_seam: %s is used in fw_lk_routes() but not "
                     "#defined in include/proto.h" % n)
        out[n] = int(m.group(1), 16)
    return out


def lk_cases(lk_c, lk_h):
    body = lk_c[lk_c.index("void lk_loop("):]
    body = body[:body.index("\n/****")]
    names = set(re.findall(r"case\s+(LK_[A-Z0-9_]+)\s*:", strip_comments(body)))
    out = {}
    for n in names:
        m = re.search(r"#define\s+%s\s+0x([0-9A-Fa-f]{2})" % n, lk_h)
        if m:
            out[n] = int(m.group(1), 16)
    return out


# Our framer declares each opcode's wire shape (arg_len() plus payload_len());
# LK pulls its own arguments through CONN_RECV. Nothing makes the two agree,
# and one byte of disagreement desynchronises the stream: the leftover is read
# as the next opcode. Both sides are extracted mechanically as symbolic counts
# and `arg[N]` is the host byte at offset N after the opcode. Anything
# undecidable is a per-opcode failure, never assumed to match: a receive inside
# an `if`, `while` or `else`, a loop bound that is neither a stream byte nor a
# mirrored variable, or a call into an LK helper that receives on its own
# account (lk_agb_cart_read_data_3d_memory(), LK.c:1753).


class Undetermined(Exception):
    pass


_RECV_U = re.compile(r"lk_conn_recv_u(8|16|32)\s*\(\s*\)")
_RECV_BUF = re.compile(r"(?<![A-Za-z0-9_])lk_conn_recv\s*\(")
_CTRL = re.compile(r"(?<![A-Za-z0-9_])(for|while|if|switch)\s*\(")
_BIND = re.compile(r"(?:u8|u16|u32|s8|s16|s32|bool)\s+(\w+)\s*=\s*$")
_CAST = re.compile(r"^\(\s*(?:u|s)?(?:int)?\d+(?:_t)?\s*\)\s*")


def _close(s, i, open_ch, close_ch):
    """Index of the bracket closing the one at s[i]."""
    depth = 0
    while i < len(s):
        if s[i] == open_ch:
            depth += 1
        elif s[i] == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise Undetermined("unbalanced %s" % open_ch)


def _open(s, i):
    """Index of the '(' matching the ')' at s[i]."""
    depth = 0
    while i >= 0:
        if s[i] == ")":
            depth += 1
        elif s[i] == "(":
            depth -= 1
            if depth == 0:
                return i
        i -= 1
    raise Undetermined("unbalanced )")


class Bytes(object):
    """A byte count as `const + sum(coeff * symbol)`."""

    def __init__(self, const=0, syms=None):
        self.const = const
        self.syms = {k: v for k, v in (syms or {}).items() if v}

    def plus(self, o):
        s = dict(self.syms)
        for k, v in o.syms.items():
            s[k] = s.get(k, 0) + v
        return Bytes(self.const + o.const, s)

    def scaled(self, by):
        """Multiply. One side must be constant; `num * TRANSFER_SIZE` is not
        something either dispatcher can mean."""
        if self.syms and by.syms:
            raise Undetermined("a count multiplied by two variables")
        if not by.syms:
            return Bytes(self.const * by.const,
                         {k: v * by.const for k, v in self.syms.items()})
        return by.scaled(self)

    def __eq__(self, o):
        return self.const == o.const and self.syms == o.syms

    def __str__(self):
        parts = [str(self.const)] if (self.const or not self.syms) else []
        for k in sorted(self.syms):
            parts.append(k if self.syms[k] == 1 else "%d*%s" % (self.syms[k], k))
        return " + ".join(parts)


def _value(expr, binds):
    e = expr.strip().rstrip(";").strip()
    e = _CAST.sub("", e)
    while e.startswith("(") and _close(e, 0, "(", ")") == len(e) - 1:
        e = _CAST.sub("", e[1:-1].strip())
    if re.match(r"^0[xX][0-9A-Fa-f]+u?$", e):
        return Bytes(int(e.rstrip("uU"), 16))
    if re.match(r"^\d+u?$", e):
        return Bytes(int(e.rstrip("uU")))
    # These two spellings are one symbol only because mirror_to_lk() copies one
    # into the other; main() asserts that assignment still exists.
    if e in ("_lk_var16[LK_VAR16_TRANSFER_SIZE]", "st->transfer_size"):
        return Bytes(0, {"TRANSFER_SIZE": 1})
    if e in binds:
        return Bytes(0, {binds[e]: 1})
    m = re.match(r"^P\.arg\[(\d+)\]$", e)
    if m:
        return Bytes(0, {"arg[%s]" % m.group(1): 1})
    m = re.match(r"^([^*]+?)\s*\*\s*(.+)$", e)
    if m:
        return _value(m.group(1), binds).scaled(_value(m.group(2), binds))
    raise Undetermined("cannot evaluate `%s`" % e)


def _blocks(body):
    """Every braced block, as (lo, hi, kind, header). `kind` is the control
    keyword that owns it, or None for a plain block."""
    out, stack = [], []
    for i, ch in enumerate(body):
        if ch == "{":
            stack.append(i)
        elif ch == "}":
            if not stack:
                raise Undetermined("unbalanced }")
            lo = stack.pop()
            kind = header = None
            j = lo - 1
            while j >= 0 and body[j] in " \t\r\n":
                j -= 1
            if j >= 0 and body[j] == ")":
                op = _open(body, j)
                header = body[op + 1:j]
                k = op - 1
                while k >= 0 and body[k] in " \t\r\n":
                    k -= 1
                m = re.search(r"(\w+)$", body[:k + 1])
                if m and m.group(1) in ("for", "while", "if", "switch"):
                    kind = m.group(1)
            else:
                m = re.search(r"(\w+)$", body[:j + 1])
                if m and m.group(1) in ("else", "do"):
                    kind = "else" if m.group(1) == "else" else "while"
            out.append((lo, i, kind, header))
    if stack:
        raise Undetermined("unbalanced {")
    return out


def _braceless(body):
    """Control statements with no braces: `if (x) break;`. Same tuple shape."""
    out = []
    for m in _CTRL.finditer(body):
        close = _close(body, m.end() - 1, "(", ")")
        p = close + 1
        while p < len(body) and body[p] in " \t\r\n":
            p += 1
        if p < len(body) and body[p] != "{":
            end = body.index(";", p) + 1
            out.append((p, end, m.group(1), body[m.end():close]))
    for m in re.finditer(r"(?<![A-Za-z0-9_])else(?![A-Za-z0-9_])", body):
        p = m.end()
        while p < len(body) and body[p] in " \t\r\n":
            p += 1
        if p < len(body) and body[p] != "{" and not body[p:].startswith("if"):
            out.append((p, body.index(";", p) + 1, "else", ""))
    return out


def lk_consumed(body, recv_fns):
    """How many bytes LK.c's case body pulls off the wire, symbolically."""
    for fn in sorted(recv_fns):
        if re.search(r"(?<![A-Za-z0-9_])%s\s*\(" % re.escape(fn), body):
            raise Undetermined("calls %s(), which receives on its own account"
                               % fn)

    spans = _blocks(body) + _braceless(body)
    events = [(m.start(), m.end(), int(m.group(1)) // 8, None)
              for m in _RECV_U.finditer(body)]
    for m in _RECV_BUF.finditer(body):
        close = _close(body, m.end() - 1, "(", ")")
        args = body[m.end():close]
        if args.count(",") != 1:
            raise Undetermined("lk_conn_recv(%s)" % args.strip())
        events.append((m.start(), close + 1, None, args.split(",")[1]))
    events.sort()

    total, binds = Bytes(), {}
    for start, end, size, count_expr in events:
        loops = []
        for lo, hi, kind, header in spans:
            if lo <= start < hi:
                if kind in ("if", "while", "switch", "else"):
                    raise Undetermined("a receive inside a `%s`" % kind)
                if kind == "for":
                    loops.append(header)
        base = Bytes(size) if size is not None else _value(count_expr, binds)
        if not loops and not total.syms:
            b = _BIND.search(body[max(0, start - 80):start])
            if b:
                binds[b.group(1)] = "arg[%d]" % total.const
        for header in loops:
            cond = header.split(";")
            if len(cond) != 3 or "<" not in cond[1]:
                raise Undetermined("a `for` header this cannot read: `%s`"
                                   % header.strip())
            base = base.scaled(_value(cond[1].split("<", 1)[1], binds))
        total = total.plus(base)
    return total


def lk_case_bodies(lk_c):
    """LK_CMD name -> the source of its case in lk_loop(); fall-through labels
    share the body they fall into."""
    body = lk_c[lk_c.index("void lk_loop("):]
    # Slice before stripping comments: the end marker is a comment banner.
    body = strip_comments(body[:body.index("\n/****")])
    marks = list(re.finditer(
        r"^[ \t]*(?:case[ \t]+(LK_[A-Z0-9_]+)[ \t]*:|default[ \t]*:)", body, re.M))
    out, pending = {}, []
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(body)
        text = body[m.end():end]
        if m.group(1):
            pending.append(m.group(1))
        if text.strip():
            for n in pending:
                out[n] = text
            pending = []
    return out


def lk_recv_functions(lk_c):
    """LK.c functions that receive, other than lk_loop and the primitives.
    Transitive: a helper that merely calls one is caught too."""
    src = strip_comments(lk_c)
    bodies = {}
    for m in re.finditer(r"^(?:static\s+)?[A-Za-z_][\w \t\*]*?(\w+)\s*\([^;{]*\)\s*\{",
                         src, re.M):
        i = src.index("{", m.end() - 1)
        bodies[m.group(1)] = src[i:_close(src, i, "{", "}")]
    prim = {"lk_loop", "lk_conn_recv", "lk_conn_recv_u8", "lk_conn_recv_u16",
            "lk_conn_recv_u32"}
    out = {n for n, b in bodies.items()
           if n not in prim and re.search(r"lk_conn_recv(_u\d+)?\s*\(", b)}
    while True:
        more = {n for n, b in bodies.items()
                if n not in prim and n not in out
                and any(re.search(r"(?<![A-Za-z0-9_])%s\s*\(" % f, b) for f in out)}
        if not more:
            return out
        out |= more


def our_declared(proto_c, proto_h):
    """CMD name -> the Bytes our framer declares: arg_len() + payload_len()."""
    def switch_of(sig):
        s = proto_c[proto_c.index(sig):]
        return strip_comments(s[:s.index("\n}\n")])

    args = dict((m.group(1), int(m.group(2))) for m in re.finditer(
        r"case\s+(CMD_[A-Z0-9_]+)\s*:\s*return\s+(\d+)u?\s*;",
        switch_of("static uint8_t arg_len(")))

    pay, pending = {}, []
    for m in re.finditer(r"case\s+(CMD_[A-Z0-9_]+)\s*:|return\s+([^;]+);",
                         switch_of("static uint16_t payload_len(")):
        if m.group(1):
            pending.append(m.group(1))
        else:
            for n in pending:
                pay[n] = m.group(2)
            pending = []

    vs = re.search(r"#define\s+FW_VAR_STATE_LEN\s+(\d+)", proto_h)
    out = {}
    for n in set(args) | set(pay):
        e = pay.get(n, "0u")
        if vs:
            e = e.replace("FW_VAR_STATE_LEN", vs.group(1))
        out[n] = Bytes(args.get(n, 0)).plus(_value(e, {}))
    return out


def main():
    lk_c, lk_h = read(LKC), read(LKH)
    glue, glue_h = read(GLUE), read(GLUE_H)
    dev_h, proto_h, proto_c, main_c = read(DEV_H), read(PROTO_H), read(PROTO_C), read(MAIN_C)
    mk = read(MAKEFILE)

    routed = routed_opcodes(glue, proto_h)
    lkc = lk_cases(lk_c, lk_h)

    print("\nevery routed opcode is one LK.c actually handles")
    print("  routed: %s" % ", ".join("0x%02X" % v for v in sorted(routed.values())))
    for name, op in sorted(routed.items(), key=lambda kv: kv[1]):
        ck(op in lkc.values(),
           "0x%02X (%s) has a case in lk_loop()" % (op, name),
           "lk_loop()'s default answers LK_STATUS_ERROR and consumes no "
           "arguments, so routing an opcode LK does not know turns this "
           "command's arguments into the next command's opcodes.")

    print("\nour framer and LK.c agree on how many bytes each routed opcode is")
    ck(re.search(r"_lk_var16\[LK_VAR16_TRANSFER_SIZE\]\s*=\s*st->transfer_size;",
                 glue) is not None,
       "TRANSFER_SIZE means the same number on both sides",
       "the comparison below treats LK's _lk_var16[LK_VAR16_TRANSFER_SIZE] and "
       "our st->transfer_size as one symbol. That is only true while "
       "mirror_to_lk() copies one into the other.")
    bodies = lk_case_bodies(lk_c)
    recv_fns = lk_recv_functions(lk_c)
    ours = our_declared(proto_c, proto_h)
    lk_name_of = dict((v, k) for k, v in lkc.items())
    for name, op in sorted(routed.items(), key=lambda kv: kv[1]):
        lk_name = lk_name_of.get(op)
        want = ours.get(name, Bytes(0))
        if lk_name is None or lk_name not in bodies:
            ck(False, "0x%02X (%s): no case body found in lk_loop()" % (op, name))
            continue
        try:
            got = lk_consumed(bodies[lk_name], recv_fns)
        except Undetermined as e:
            ck(False,
               "0x%02X (%s): LK's byte count could not be determined statically"
               % (op, name),
               "%s\n         Our framer declares %s for it. Read LK.c's case by "
               "hand and either widen this analysis or stop routing the opcode. "
               "An unverified length is how a stream desynchronises."
               % (e, want))
            continue
        ck(want == got,
           "0x%02X (%s): framer declares %-14s LK consumes %s"
           % (op, name, str(want) + ",", got),
           "A MISMATCH DESYNCHRONISES THE STREAM. Our framer hands LK the "
           "bytes it declared and LK reads what it wants; the difference "
           "becomes the next opcode.")

    print("\nthe front end still bounds what LK will not")
    # The literal must track lk_admit()'s signature, or this fails for a reason
    # unrelated to the ordering it protects.
    ck("lk_admit(st, op)" in glue and
       glue.index("lk_admit(st, op)") < glue.index("lk_loop(op);"),
       "fw_lk_dispatch() admits the command before lk_loop() runs",
       "LK.c:541-545 reads an unbounded host count into _lk_flashcmd_*[x+16]; "
       "proto.c refuses an over-long batch and routing 0xD4 made that refusal "
       "unreachable.")
    base = re.search(r"_lk_flashcmd_addr\[x\+(\d+)\]", strip_comments(lk_c))
    ours_base = re.search(r"#define\s+FW_LK_BATCH_BASE\s+(\d+)u", glue)
    ck(base is not None and ours_base is not None
       and int(base.group(1)) == int(ours_base.group(1)),
       "FW_LK_BATCH_BASE still equals LK.c's own index offset (%s)"
       % (base.group(1) if base else "?"),
       "the bound is `array size - this offset`; if upstream moves the offset "
       "and the constant stays, the bound is quietly wrong in the unsafe "
       "direction.")

    print("\nthe carve-outs are still carved out")
    ck(0xD3 not in routed.values(),
       "FLASH_PROGRAM (0xD3) is NOT routed",
       "LK's lk_conn_recv() is blocking by construction (LK.c:928-930; "
       "FLASH_PROGRAM's own call is LK.c:595), which forecloses "
       "agb_stream_pump() (~47% of write throughput at 6 Mbaud). "
       "This is the explicit carve-out of the stage 1 port.")
    for op, why in ((0xE0, "CMD_FW_BENCH_TX"), (0xE1, "CMD_FW_SET_STEP"),
                    (0xE6, "CMD_FW_DUMP_PAYLOAD"), (0xE7, "CMD_FW_ECHO_PAYLOAD")):
        ck(op not in routed.values(),
           "diagnostic 0x%02X (%s) is NOT routed" % (op, why),
           "not in the host's DEVICE_CMD table at all; lk_loop() would answer "
           "LK_STATUS_ERROR. These are how the transport was measured.")
    ck(0xC8 not in routed.values(),
       "3D Memory (0xC8) is NOT routed",
       "lk_agb_cart_read_data_3d_memory() consumes the client ack itself "
       "(LK.c:1752-1754). Routing it while proto.h's page_terminator_due rule "
       "still swallows that byte is a HANG, not a cosmetic leftover.")
    ck("if (st->mode != FW_MODE_DMG)" in glue,
       "routing is gated on DMG mode",
       "In AGB mode this port must change nothing, so that the first hardware "
       "regression is 'did DMG change?' and not 'did anything change?'.")

    print("\nthe opcode boundary is enforced before LK ever sees a byte")
    ck("int fw_proto_idle(void)" in proto_c and "fw_proto_idle(void);" in proto_h,
       "fw_proto_idle() exists")
    ck(re.search(r"return\s*\(P\.op == 0u\)\s*&&\s*\(P\.pay_need == 0u\)", proto_c)
       is not None,
       "fw_proto_idle() checks BOTH the opcode and the payload phase",
       "a FLASH_PROGRAM collecting 2048 payload bytes has P.op set AND "
       "pay_need set; checking only one lets ROM data be dispatched as a "
       "command.")
    ck(re.search(r"if \(fw_proto_idle\(\) && fw_lk_routes\(&g_state, rx\[i\]\)\)",
                 main_c) is not None,
       "fw_main() dispatches to LK only when the parser is idle")

    print("\nthe state mirror covers every variable LK reads")
    used = set(re.findall(r"_lk_var8\[(LK_VAR8_[A-Z0-9_]+)\]", strip_comments(lk_c)))
    used |= set(re.findall(r"_lk_var16\[(LK_VAR16_[A-Z0-9_]+)\]", strip_comments(lk_c)))
    used |= set(re.findall(r"_lk_var32\[(LK_VAR32_[A-Z0-9_]+)\]", strip_comments(lk_c)))
    mirror = glue[glue.index("static void mirror_to_lk("):]
    mirror = mirror[:mirror.index("\n}\n")]
    OMITTED = {
        "LK_VAR32_AUTO_POWEROFF_TIME":
            "auto power-off is not wired up; lk_cart_power_off_proc() is never "
            "called and the cell's 0 is also its value when the host leaves the "
            "feature off",
    }
    missing = sorted(v for v in used if v not in mirror and v not in OMITTED)
    ck(not missing,
       "%d LK variables are read by LK.c; all are written by mirror_to_lk() "
       "or declared omitted" % len(used),
       "not mirrored: " + ", ".join(missing) + "\n         A variable LK reads "
       "but nothing mirrors holds a stale value and produces wrong bytes on the "
       "cartridge with no error anywhere.")
    stale = sorted(v for v in OMITTED if v in mirror)
    ck(not stale, "no deliberate omission has quietly acquired a mirror",
       ", ".join(stale))
    ck("flash_write_cycle" in mirror,
       "flash_write_cycle[] is mirrored too",
       "lk_dmg_agb_flash_unbuffered() reads flash_write_cycle, not "
       "_lk_flashcmd_*; omitting it replays zeroes as the unlock sequence.")
    ck("_lk_bankcmd_num" in mirror and re.search(r"if \(n > 3u\)", mirror),
       "the bank-change count is clamped to LK's 3-entry table",
       "_lk_bankcmd_addr[3] (LK.h) vs FW_BANK_CMD_MAX 8 (proto.h). An "
       "unclamped 4 is four writes off the end of a three-element array.")

    print("\nthe mirror does NOT copy CART_MODE back, which is the 5 V interlock")
    back = glue[glue.index("static void mirror_from_lk("):]
    back = back[:back.index("\n}\n")]
    ck("LK_VAR8_CART_MODE" not in back,
       "mirror_from_lk() leaves st->mode alone",
       "LK.c:190-194 force-sets _lk_var8[LK_VAR8_CART_MODE] = LK_MODE_DMG on "
       "the line BEFORE SET_VOLTAGE_5V(). Copying it back would make "
       "fw_cart_voltage()'s DMG-only guard always pass, an interlock that "
       "looks present and is inert. See patches/0004-front-end-interlocks.md.")
    ck("fw_cart_voltage(1, dmg)" in glue,
       "fw_lk_voltage_5v() still goes through fw_cart_voltage()",
       "the guard must stay in the function host/test_cart.py asserts on.")
    ck("g_st->mode == FW_MODE_DMG" in glue,
       "and it decides on OUR mode, not LK's variable")

    print("\nthe power-on settle reaches every routed command")
    disp = glue[glue.index("uint32_t fw_lk_dispatch("):]
    ck("fw_cart_wait_ready();" in disp and
       disp.index("fw_cart_wait_ready();") < disp.index("lk_loop(op);"),
       "fw_lk_dispatch() waits for the cartridge BEFORE lk_loop()",
       "FW_CART_POWERON_MS is 300 and 200 ms of it is deferred to the first "
       "bus access (FW_CART_POWERON_ACK_MS, cart.h). lk_cart_power_on() is "
       "straight-line and cannot host the split.")
    ck("void fw_cart_wait_ready(void)" in main_c,
       "main.c exports it")

    print("\nthe carried patches are wired, guarded, and still apply")
    pdir = os.path.join(ROOT, "patches")
    listed = re.findall(r"(patches/\d{4}-[a-z0-9-]+\.patch)", mk)
    on_disk = sorted("patches/" + f for f in os.listdir(pdir) if f.endswith(".patch"))
    ck(sorted(set(listed)) == on_disk,
       "every .patch on disk is in the Makefile's LK_PATCHES",
       "Makefile: %s\n         on disk: %s" % (sorted(set(listed)), on_disk))
    for p in on_disk:
        pid = os.path.basename(p).split("-")[0]
        ck("FW_LK_PATCH_%s" % pid in dev_h,
           "%s is guarded by FW_LK_PATCH_%s in the board header" % (p, pid),
           "without the guard, a patch that stops applying changes the device "
           "silently instead of failing the build.")
    ck("$(addprefix -DFW_LK_PATCH_,$(LK_PATCH_IDS))" in mk,
       "the -D list is derived from LK_PATCHES, not written out twice",
       "two lists drift; one cannot.")

    if shutil.which("patch"):
        tmp = tempfile.mkdtemp()
        try:
            shutil.copy(LKC, os.path.join(tmp, "LK.c"))
            ok = True
            for p in on_disk:
                # --fuzz=0: at the default fuzz a mutated context line still
                # applies, which defeats the check.
                r = subprocess.run(["patch", "--forward", "--batch",
                                    "--fuzz=0", "-p1",
                                    "-d", tmp, "-i", os.path.join(ROOT, p)],
                                   capture_output=True, text=True)
                if r.returncode != 0:
                    ok = False
                    print("         %s: %s" % (p, r.stdout.strip().splitlines()[-1:]))
            ck(ok, "all %d patches apply cleanly to the pinned LK.c" % len(on_disk),
               "upstream moved under a carried fix. See patches/README.md")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    else:
        print("  [SKIP] patch(1) not found; cannot verify the patches apply")

    print("\nthe vendored sources were not edited in place")
    ck("_delay_agb_latch" not in lk_c and "AGB_SAVE_FLASH_WAIT" not in lk_c
       and "carried patch" not in lk_c,
       "upstream/FlashGBX_LK_Firmware/LK.c carries none of our changes",
       "the whole value of the port is that upgrading stays a three-file swap.")

    print("\ntest_lk_seam: %d passed, %d failed" % (_pass, _fail))
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
