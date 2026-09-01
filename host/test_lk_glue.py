#!/usr/bin/env python3
"""src/lk_glue.c, compiled and called on the host against a model of LK.c.

The bound: LK.c:541-545 reads a host byte `num` and writes
_lk_flashcmd_addr[x+16] for x in [0,num) into a 32-entry array. num=17 lands on
_lk_var8[CART_MODE], the cartridge's bus mode; num=255 lands past __bss_end.
Nothing in LK.c bounds it; lk_admit() (src/lk_glue.c) does, and this proves it
is on the path.

lk_loop() here is a model transcribed from LK.c (host/lk_glue_shim.c). No
cartridge and no USB: this says what the front end does with a byte stream,
never what upstream does with a bus.
"""

import ctypes
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_checks = 0
_fails = 0


def ck(cond, what, detail=""):
    global _checks, _fails
    _checks += 1
    if not cond:
        _fails += 1
        print("  FAIL %s%s" % (what, ("  [%s]" % detail) if detail else ""))
    return bool(cond)


def section(name):
    print(name)


def const(path, name, default=None):
    """A #define'd integer read out of a header, so the test cannot drift from
    the firmware's own value."""
    src = open(os.path.join(ROOT, path)).read()
    m = re.search(r"^#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)u?\s*$" % name, src, re.M)
    if not m:
        if default is not None:
            return default
        sys.exit("test_lk_glue: %s is not #defined in %s" % (name, path))
    return int(m.group(1), 0)


ACK_OK = const("include/proto.h", "ACK_OK")
ACK_ERROR = const("include/proto.h", "ACK_ERROR")
FW_BATCH_MAX = const("include/proto.h", "FW_BATCH_MAX")
FW_MODE_DMG = 1

# LK's own numbers, so a resize upstream shows up as a changed bound rather
# than a stale literal.
LKH = open(os.path.join(ROOT, "upstream", "FlashGBX_LK_Firmware", "LK.h")).read()
LK_FLASHCMD_N = int(re.search(r"_lk_flashcmd_addr\[(\d+)\]", LKH).group(1))
LKC = open(os.path.join(ROOT, "upstream", "FlashGBX_LK_Firmware", "LK.c")).read()
LK_BATCH_BASE = int(re.search(r"_lk_flashcmd_addr\[x\+(\d+)\]", LKC).group(1))

# Largest batch the routed path accepts: LK's array minus LK's offset, capped
# by FW_BATCH_MAX. src/lk_glue.c derives the same value in C.
ADMIT = min(LK_FLASHCMD_N - LK_BATCH_BASE, FW_BATCH_MAX)


def build(tmp):
    so = os.path.join(tmp, "liblkglue.so")
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-std=c99", "-g", "-O1", "-fPIC", "-shared",
           "-Wall", "-Wextra", "-Wno-unused-parameter",
           # LKDEV_REG32 is overridable for this; see the board header's
           # MISMATCH note and fw_lk_led_idle().
           "-DLKDEV_REG32(a)=(*fw_test_reg((uintptr_t)(a)))",
           "-include", os.path.join(ROOT, "host", "mmio_shim.h"),
           # The board header #errors without these.
           "-DFW_LK_PATCH_0001", "-DFW_LK_PATCH_0002", "-DFW_LK_PATCH_0003",
           "-DFW_TIMESTAMP=1755388800", "-DBL_USB_ECHO=0",
           "-DLK_DEVICE_HEADER=\"LK_device_ch579.h\"",
           "-I", os.path.join(ROOT, "include"),
           "-I", os.path.join(ROOT, "host"),
           "-I", os.path.join(ROOT, "upstream", "FlashGBX_LK_Firmware"),
           "-o", so,
           os.path.join(ROOT, "src", "lk_glue.c"),
           os.path.join(ROOT, "host", "lk_glue_shim.c"),
           os.path.join(ROOT, "host", "mmio_shim.c")]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode != 0:
        sys.stdout.write(p.stdout.decode("utf-8", "replace"))
        sys.exit("could not build src/lk_glue.c for the host")
    return so


class Glue(object):
    def __init__(self, so):
        self.lib = ctypes.CDLL(so)
        u32 = ctypes.c_uint32
        self.lib.fw_lk_dispatch.restype = u32
        self.lib.fw_lk_dispatch.argtypes = [ctypes.c_void_p, ctypes.c_uint8,
                                            ctypes.c_char_p, u32]
        self.lib.fw_lk_routes.restype = ctypes.c_int
        self.lib.fw_lk_routes.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
        self.lib.fw_lk_recv_truncated.restype = ctypes.c_int
        self.lib.fw_lk_send_stalled.restype = ctypes.c_int
        self.lib.shim_state.restype = ctypes.c_void_p
        for n in ("shim_tx_len", "shim_tx_at", "shim_rx_unread", "shim_lk_calls",
                  "shim_lk_last_op", "shim_batch_num", "shim_batch_flash",
                  "shim_overflow", "shim_overflow_index", "shim_cart_bytes",
                  "shim_cart_len", "shim_cart_at", "shim_wait_ready",
                  "shim_flashcmd_addr", "shim_flashcmd_data"):
            getattr(self.lib, n).restype = u32

    def reset(self, mode=FW_MODE_DMG, transfer_size=0):
        self.lib.shim_reset()
        self.lib.shim_state_mode(ctypes.c_int(mode))
        self.lib.shim_state_transfer_size(ctypes.c_uint32(transfer_size))

    def feed(self, data):
        b = bytes(data)
        self.lib.shim_feed(b, ctypes.c_uint32(len(b)))

    def dispatch(self, op, tail=b""):
        st = self.lib.shim_state()
        return self.lib.fw_lk_dispatch(st, ctypes.c_uint8(op),
                                       bytes(tail), ctypes.c_uint32(len(tail)))

    def routes(self, op):
        return self.lib.fw_lk_routes(self.lib.shim_state(), ctypes.c_uint8(op))

    def tx(self):
        n = self.lib.shim_tx_len()
        return bytes(self.lib.shim_tx_at(i) & 0xFF for i in range(n))


def batch(num, flashcart=1, pairs=None, hdr_count=None):
    """A CART_WRITE_FLASH_CMD body: flashcart, count, then count x (addr32,
    val16). `hdr_count` overrides the declared count without changing how many
    pairs follow, for the truncation cases."""
    if pairs is None:
        pairs = [(0x00000AAA + i, 0x00A0 + i) for i in range(num)]
    out = bytearray([flashcart, num if hdr_count is None else hdr_count])
    for a, v in pairs:
        out += bytes([(a >> 24) & 0xFF, (a >> 16) & 0xFF,
                      (a >> 8) & 0xFF, a & 0xFF,
                      (v >> 8) & 0xFF, v & 0xFF])
    return bytes(out)


def test_a_legal_batch_still_reaches_lk(g):
    section("CART_WRITE_FLASH_CMD: a batch LK can hold goes through unchanged")
    for n in (0, 1, 3, ADMIT):
        body = batch(n)
        g.reset(transfer_size=0)
        used = g.dispatch(0xD4, body)
        ck(g.lib.shim_lk_calls() == 1, "n=%d: lk_loop() ran" % n)
        ck(g.lib.shim_lk_last_op() == 0xD4, "n=%d: with 0xD4" % n)
        ck(g.lib.shim_batch_num() == n, "n=%d: LK read the same count" % n,
           g.lib.shim_batch_num())
        ck(g.lib.shim_batch_flash() == 1,
           "n=%d: LK read the flashcart selector too" % n)
        ck(used == len(body), "n=%d: the whole body was consumed" % n,
           "%d of %d" % (used, len(body)))
        ck(g.tx() == bytes([ACK_OK]), "n=%d: LK's own OK reached the host" % n,
           g.tx().hex())
        ck(g.lib.shim_overflow() == 0, "n=%d: no out-of-range store" % n)
        ok = all(g.lib.shim_flashcmd_addr(LK_BATCH_BASE + i) == 0x00000AAA + i
                 and g.lib.shim_flashcmd_data(LK_BATCH_BASE + i) == 0x00A0 + i
                 for i in range(n))
        ck(ok, "n=%d: every pair landed at _lk_flashcmd_*[x+%d]"
           % (n, LK_BATCH_BASE))


def test_the_header_is_pushed_back_not_swallowed(g):
    section("the two header bytes the front end reads are given back to LK")
    # The tail carries only the opcode's first byte. Drop the pushback and LK
    # reads the count as the flashcart selector and the first address byte as
    # the count.
    body = batch(2)
    g.reset()
    g.feed(body[1:])
    used = g.dispatch(0xD4, body[:1])
    ck(used == 1, "only the byte that was in the tail is reported consumed",
       used)
    ck(g.lib.shim_batch_flash() == body[0] and g.lib.shim_batch_num() == 2,
       "LK saw flashcart=%d count=2 across the tail/endpoint boundary"
       % body[0],
       "%d/%d" % (g.lib.shim_batch_flash(), g.lib.shim_batch_num()))
    ck(g.lib.shim_rx_unread() == 0, "and consumed every queued byte",
       g.lib.shim_rx_unread())
    ck(g.tx() == bytes([ACK_OK]), "reply is OK", g.tx().hex())


def test_an_oversized_batch_is_refused(g):
    section("a batch LK cannot hold is REFUSED, not truncated to fit")
    # ADMIT+1 is the first count whose last store is out of range; 255 is the
    # largest a u8 can carry. Neither is bounded anywhere in LK.c.
    for n in (ADMIT + 1, ADMIT + 2, 64, 255):
        body = batch(n)
        g.reset()
        used = g.dispatch(0xD4, body)
        ck(g.lib.shim_lk_calls() == 0,
           "n=%d: lk_loop() never ran" % n, g.lib.shim_lk_calls())
        ck(g.lib.shim_overflow() == 0,
           "n=%d: nothing was written past _lk_flashcmd_*[%d]"
           % (n, LK_FLASHCMD_N - 1),
           "%d stores, first at index %d"
           % (g.lib.shim_overflow(), g.lib.shim_overflow_index()))
        ck(g.tx() == bytes([ACK_ERROR]),
           "n=%d: one ACK_ERROR, the same byte proto.c answers" % n,
           g.tx().hex())
        ck(used == len(body),
           "n=%d: the declared payload was consumed, so the next byte on the "
           "wire is the next OPCODE" % n, "%d of %d" % (used, len(body)))
        ck(g.lib.shim_wait_ready() == 0,
           "n=%d: a refused command did not wait for the cartridge" % n)


def test_the_refusal_does_not_depend_on_the_payload_arriving(g):
    section("a bogus count with nothing behind it is still refused, and bounded")
    # The shape a fuzzer or a crashed host produces: `D4 01 FF` and silence.
    g.reset()
    used = g.dispatch(0xD4, bytes([1, 255]))
    ck(g.lib.shim_lk_calls() == 0, "lk_loop() never ran")
    ck(g.tx() == bytes([ACK_ERROR]), "still answered ACK_ERROR", g.tx().hex())
    ck(g.lib.fw_lk_recv_truncated() != 0,
       "and the drain reported that the payload never came")
    ck(used == 2, "the two header bytes in the tail were consumed", used)


def test_the_bound_is_lks_capacity_not_ours(g):
    section("the routed bound comes from LK's array, not from FW_BATCH_MAX")
    # Our own dispatcher accepts FW_BATCH_MAX pairs, LK holds
    # LK_FLASHCMD_N - LK_BATCH_BASE; routing takes the smaller. PORT-NOTES.md
    # records the resulting behaviour change.
    ck(ADMIT == LK_FLASHCMD_N - LK_BATCH_BASE,
       "LK holds %d pairs (u32[%d] indexed at x+%d) and that is the bound"
       % (ADMIT, LK_FLASHCMD_N, LK_BATCH_BASE))
    if FW_BATCH_MAX > ADMIT:
        g.reset()
        g.dispatch(0xD4, batch(FW_BATCH_MAX))
        ck(g.lib.shim_lk_calls() == 0 and g.tx() == bytes([ACK_ERROR]),
           "a batch of %d (legal for proto.c, too long for LK) is refused "
           "on the routed path" % FW_BATCH_MAX, g.tx().hex())


def test_the_bound_does_not_leak_into_the_next_command(g):
    section("the pushback is per-command")
    g.reset()
    g.dispatch(0xD4, batch(2))
    # 0xB2 takes addr32 + value8 and no payload. A surviving pushback from the
    # previous command would be read as the top half of the address.
    g.dispatch(0xB2, bytes([0x00, 0x00, 0x21, 0x00, 0x05]))
    ck(g.lib.shim_cart_at(0) == 0x21 and g.lib.shim_cart_at(1) == 0x05,
       "DMG_CART_WRITE read its own arguments",
       "%02X %02X" % (g.lib.shim_cart_at(0), g.lib.shim_cart_at(1)))


def test_a_truncated_payload_is_not_acked(g):
    section("a routed write the host abandoned reports ACK_ERROR, not OK")
    # DMG_CART_WRITE_SRAM: no arguments, TRANSFER_SIZE bytes of payload. Send
    # half and stop. Upstream writes the buffer to the cartridge and answers
    # LK_STATUS_OK regardless, because lk_conn_recv() is void.
    g.reset(transfer_size=64)
    g.dispatch(0xB3, bytes(range(32)))
    ck(g.lib.shim_lk_calls() == 1, "lk_loop() ran (the write cannot be recalled)")
    ck(g.lib.fw_lk_recv_truncated() != 0, "the seam saw the truncation")
    ck(g.lib.shim_cart_bytes() == 64,
       "LK still pushed a full block at the cartridge, the residual this "
       "port cannot fix without editing LK.c", g.lib.shim_cart_bytes())
    ck(g.lib.shim_cart_at(32) == 0xFF and g.lib.shim_cart_at(63) == 0xFF,
       "and the missing tail was FW_LK_RECV_FILL, not the previous block",
       "%02X" % g.lib.shim_cart_at(32))
    ck(g.tx() == bytes([ACK_ERROR]),
       "THE HOST IS TOLD IT FAILED: LK's OK was dropped and replaced",
       g.tx().hex())


def test_a_truncated_read_does_not_stream_garbage(g):
    section("a routed command whose ARGUMENTS were truncated says nothing true")
    # 0xB2 wants five argument bytes. Give it two.
    g.reset()
    g.dispatch(0xB2, bytes([0x00, 0x21]))
    ck(g.lib.fw_lk_recv_truncated() != 0, "the seam saw it")
    ck(g.tx() == bytes([ACK_ERROR]), "one ACK_ERROR, no LK_STATUS_OK",
       g.tx().hex())


def test_a_stalled_host_is_not_sent_a_substitute(g):
    section("when the host stopped READING, the substitute byte is not sent")
    # Both flags at once: no payload arrives and nothing can be transmitted.
    # Without the fw_lk_send_stalled() check the front end sits through a
    # second full FW_TX_STALL_MS pushing one byte at a host that is not there.
    g.reset(transfer_size=64)
    g.lib.shim_tx_dead(ctypes.c_int(1))
    g.dispatch(0xB3, bytes(range(8)))
    ck(g.lib.fw_lk_recv_truncated() != 0, "recv gave up")
    ck(g.lib.fw_lk_send_stalled() != 0, "send gave up")
    ck(g.lib.shim_tx_len() == 0, "nothing went out", g.lib.shim_tx_len())


def test_a_complete_command_reports_neither(g):
    section("and none of that fires on a command that completed")
    g.reset(transfer_size=16)
    g.dispatch(0xB3, bytes(range(16)))
    ck(g.lib.fw_lk_recv_truncated() == 0, "no truncation")
    ck(g.lib.fw_lk_send_stalled() == 0, "no stall")
    ck(g.tx() == bytes([ACK_OK]), "and LK's own OK is what the host got",
       g.tx().hex())


def test_routing_is_gated_on_our_mode(g):
    section("fw_lk_routes() answers from st->mode, at runtime")
    g.reset(mode=FW_MODE_DMG)
    ck(all(g.routes(op) for op in (0xB1, 0xB2, 0xB3, 0xD1, 0xD4)),
       "the DMG set routes in DMG mode")
    ck(not any(g.routes(op) for op in (0xD3, 0xC1, 0xC8, 0xE0, 0xE1, 0xE6, 0xE7)),
       "FLASH_PROGRAM, the AGB set and the four diagnostics do not")
    for mode, name in ((0, "no mode chosen"), (2, "AGB")):
        g.reset(mode=mode)
        ck(not any(g.routes(op) for op in range(0x100)),
           "nothing at all routes with %s" % name)


def main():
    tmp = tempfile.mkdtemp()
    g = Glue(build(tmp))
    for fn in (test_a_legal_batch_still_reaches_lk,
               test_the_header_is_pushed_back_not_swallowed,
               test_an_oversized_batch_is_refused,
               test_the_refusal_does_not_depend_on_the_payload_arriving,
               test_the_bound_is_lks_capacity_not_ours,
               test_the_bound_does_not_leak_into_the_next_command,
               test_a_truncated_payload_is_not_acked,
               test_a_truncated_read_does_not_stream_garbage,
               test_a_stalled_host_is_not_sent_a_substitute,
               test_a_complete_command_reports_neither,
               test_routing_is_gated_on_our_mode):
        fn(g)
    print("test_lk_glue: %d checks, %d failures" % (_checks, _fails))
    return 1 if _fails else 0


if __name__ == "__main__":
    sys.exit(main())
