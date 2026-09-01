#!/usr/bin/env python3
"""Checks on the USB descriptors this device presents.

The CH340 identity (VID 0x1A86, PID 0x7523, interface class 0xFF / subclass
0x01 / protocol 0x02) selects the host driver. Windows binds CH341SER.SYS,
written for a real CH340 and its 32-byte endpoints. A 64-byte bulk IN endpoint
under that identity measured 482 kB/s against 297 kB/s on macOS over the same
16 MiB ROM, and broke Windows: the device enumerated, reported "reading
cartridge", never finished, and dropped off the bus.

FW_USB_CDC presents our own VID/PID and CDC-ACM class descriptors instead, so
the host binds usbser.sys or AppleUSBACMData, neither of which has a hardcoded
idea of this chip's endpoint size. FW_USB_CDC=0 still serves the CH340 set, so
those bytes stay identical to stock, no Windows machine being available here to
certify a deviation. The CDC set has no stock bytes to compare against and is
checked on its own terms.

The endpoint size is one fact in three places: the descriptor table,
bl_usb_ep2_pkt_in, and main.c's cartridge chunk size. When they disagree the
device declares one size and uses another, invisibly on a host that tolerates
it.
"""

import ctypes
import os
import re
import shutil
import subprocess
import sys
import tempfile

def _fn_body(src):
    """Text of the first function in src, found by brace matching.

    Delimiting on a comment banner breaks whenever the banner is removed."""
    i = src.index("{")
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[:j + 1]
    return src


def pat_lines(path):
    """The added lines of a unified diff, '+' still attached."""
    return [l for l in open(path).read().split("\n")
            if l.startswith("+") and not l.startswith("+++")]


HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
STOCK = os.path.join(FW, "..", "fw", "fw.bin")

_pass = _fail = 0


def ck(cond, what, detail=""):
    global _pass, _fail
    if cond:
        _pass += 1
        print("  [ ok ] %s" % what)
    else:
        _fail += 1
        print("  [FAIL] %s  [%s]" % (what, detail))
    return cond


def section(name):
    print("\n" + name)


def walk(desc):
    out, i = [], 0
    while i < len(desc):
        ln = desc[i]
        if ln == 0:
            break
        out.append(desc[i:i + ln])
        i += ln
    return out


def find_stock_config():
    """Locate stock's configuration descriptor by structure rather than at a
    fixed offset: a 0x09/0x02 header with a sane wTotalLength and an interface
    count of 1 or 2.
    """
    if not os.path.exists(STOCK):
        return None, None
    b = open(STOCK, "rb").read()
    for m in re.finditer(rb"\x09\x02", b):
        i = m.start()
        total = b[i + 2] | (b[i + 3] << 8)
        if 0x12 <= total <= 0x60 and b[i + 4] in (1, 2) and i + total <= len(b):
            return list(b[i:i + total]), i + 0x3E00      # flash addr
    return None, None


def endpoints(cfg):
    return {d[2]: {"attr": d[3], "mps": d[4] | (d[5] << 8), "interval": d[6]}
            for d in walk(cfg) if len(d) >= 7 and d[1] == 0x05}


def by_interface(cfg):
    """[(interface_descriptor, [descriptors that belong to it])]."""
    out = []
    for d in walk(cfg):
        if d[1] == 0x04:
            out.append((d, []))
        elif out:
            out[-1][1].append(d)
    return out


def build(tmp, cdc):
    so = os.path.join(tmp, "libdesc%d.so" % cdc)
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-std=c99", "-g", "-O1", "-fPIC", "-shared",
           "-Wall", "-Wextra", "-Wno-cpp",
           "-DBL_HOST=1", "-DFW_USB_CDC=%d" % cdc,
           "-I", os.path.join(FW, "include"),
           "-o", so, os.path.join(FW, "src", "usb_desc.c")]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode != 0:
        sys.stdout.write(p.stdout.decode("utf-8", "replace"))
        sys.exit("could not build src/usb_desc.c for the host")
    return so


def arr(lib, name, n):
    return list(bytearray((ctypes.c_uint8 * n).in_dll(lib, name)))


def const(hdr, name):
    m = re.search(r"#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)" % re.escape(name), hdr)
    return int(m.group(1), 0) if m else None


def main():
    usb_src = open(os.path.join(FW, "src", "usb.c")).read()
    usb_hdr = open(os.path.join(FW, "include", "usb.h")).read()
    main_src = open(os.path.join(FW, "src", "main.c")).read()
    desc_src = open(os.path.join(FW, "src", "usb_desc.c")).read()

    EP0_PKT = const(usb_hdr, "BL_USB_EP0_PKT")
    EP2_PKT = const(usb_hdr, "BL_USB_EP2_PKT")
    EP2_PKT_CDC = const(usb_hdr, "BL_USB_EP2_PKT_CDC")
    CFG_LEN = const(usb_hdr, "BL_USB_CDC_CFG_LEN")
    VID = const(usb_hdr, "BL_USB_CDC_VID")
    PID = const(usb_hdr, "BL_USB_CDC_PID")

    tmp = tempfile.mkdtemp(prefix="usbdesc")
    try:
        lib0 = ctypes.CDLL(build(tmp, 0))       # the shipping build
        lib1 = ctypes.CDLL(build(tmp, 1))       # FW_USB_CDC=1

        ch340 = arr(lib1, "bl_usb_desc_config", 39)
        ch340_dev = arr(lib1, "bl_usb_desc_device", 18)
        cdc = arr(lib1, "bl_usb_desc_config_cdc", CFG_LEN)
        cdc_dev = arr(lib1, "bl_usb_desc_device_cdc", 18)
        default_cfg = arr(lib0, "bl_usb_desc_config", 39)

        run(usb_src, usb_hdr, main_src, desc_src,
            EP0_PKT, EP2_PKT, EP2_PKT_CDC, CFG_LEN, VID, PID,
            lib1, ch340, ch340_dev, cdc, cdc_dev, default_cfg)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("\ntest_usb_desc: %d passed, %d failed" % (_pass, _fail))
    return 1 if _fail else 0


def run(usb_src, usb_hdr, main_src, desc_src,
        EP0_PKT, EP2_PKT, EP2_PKT_CDC, CFG_LEN, VID, PID,
        lib, ch340, ch340_dev, cdc, cdc_dev, default_cfg):

    section("the CH340 set still matches stock byte for byte")
    theirs, at = find_stock_config()
    ck(ch340 == default_cfg,
       "FW_USB_CDC changes nothing about the CH340 configuration descriptor",
       "the two builds disagree")
    if theirs is None:
        print("  [SKIP] stock fw.bin not present at %s. This is the check that"
              % STOCK)
        print("         matters most, because an FW_USB_CDC=0 build serves")
        print("         exactly these bytes.")
    else:
        print("  stock configuration descriptor found at flash 0x%04X" % at)
        ck(len(ch340) == len(theirs),
           "the descriptors are the same length",
           "ours %d, stock %d" % (len(ch340), len(theirs)))
        ours_eps, their_eps = endpoints(ch340), endpoints(theirs)
        ck(set(ours_eps) == set(their_eps),
           "the same endpoint addresses are declared",
           "ours %s, stock %s" % (sorted(ours_eps), sorted(their_eps)))
        for a in sorted(set(ours_eps) & set(their_eps)):
            ck(ours_eps[a]["mps"] == their_eps[a]["mps"],
               "EP 0x%02X wMaxPacketSize matches stock (%d)"
               % (a, their_eps[a]["mps"]),
               "ours %d, stock %d" % (ours_eps[a]["mps"], their_eps[a]["mps"]))
        ck(ch340 == theirs,
           "in fact every byte matches",
           "first difference at offset %d"
           % next((i for i in range(min(len(ch340), len(theirs)))
                   if ch340[i] != theirs[i]), -1))
    ck(ch340_dev[8] | (ch340_dev[9] << 8) == 0x1A86
       and ch340_dev[10] | (ch340_dev[11] << 8) == 0x7523,
       "and it still claims to be 1A86:7523, which is what CH341SER.SYS binds")

    section("the CH340 set is internally coherent")
    coherent(ch340, "CH340")
    ck(in_mps_offset(ch340) is not None, "and it declares a bulk IN endpoint")

    section("the endpoint size is one fact, not three")
    inits = re.findall(r"uint8_t\s+bl_usb_ep2_pkt_in\s*=\s*([A-Za-z0-9_]+)\s*;",
                       usb_src)
    ck(len(inits) == 2,
       "bl_usb_ep2_pkt_in has one initialiser per identity",
       "found %r" % (inits,))
    ck(inits == ["BL_USB_EP2_PKT_CDC", "BL_USB_EP2_PKT"],
       "the CDC arm boots at the CDC size and the other at the CH340 size",
       "found %r" % (inits,))
    ck(EP2_PKT == ch340[in_mps_offset(ch340)],
       "BL_USB_EP2_PKT agrees with the CH340 descriptor",
       "constant %s, descriptor %d" % (EP2_PKT, ch340[in_mps_offset(ch340)]))
    ck(EP2_PKT_CDC == cdc[in_mps_offset(cdc)],
       "BL_USB_EP2_PKT_CDC agrees with the CDC descriptor",
       "constant %s, descriptor %d" % (EP2_PKT_CDC, cdc[in_mps_offset(cdc)]))
    ck(const(usb_hdr, "BL_USB_EP2_PKT_IN_MAX") == 64,
       "and 64 is still declared as the architectural maximum")

    # The cartridge chunk size is the bulk IN packet size, and main.c must
    # derive it rather than restate it. Drift between the two cost 27% on every
    # read: the endpoint went back to 32 for Windows and the chunk size stayed
    # at 64.
    #
    # Check every assignment to cart_step rather than searching for one shape
    # of code. A literal is allowed only under FW_USB_IRQ, where the chunk is
    # not the packet size.
    assigns = []
    depth_irq = 0
    stack = []
    for lineno, line in enumerate(main_src.split("\n"), 1):
        t = line.strip()
        if t.startswith("#if"):
            stack.append("FW_USB_IRQ" in t)
            depth_irq += stack[-1]
        elif t.startswith("#elif"):
            if stack:
                depth_irq -= stack[-1]
                stack[-1] = "FW_USB_IRQ" in t
                depth_irq += stack[-1]
        elif t.startswith("#else"):
            if stack:
                depth_irq -= stack[-1]
                stack[-1] = False
        elif t.startswith("#endif"):
            if stack:
                depth_irq -= stack.pop()
        m = re.match(r"g_state\.cart_step\s*=\s*(.+?)\s*;", t)
        if m:
            assigns.append((lineno, m.group(1), depth_irq > 0))

    ck(len(assigns) >= 2, "main.c assigns cart_step where it is expected to",
       "found %d assignments" % len(assigns))
    bad = ["line %d: %s" % (n, e) for n, e, in_irq in assigns
           if e != "bl_usb_ep2_pkt_in" and not in_irq]
    ck(not bad,
       "every cart_step assignment outside FW_USB_IRQ DERIVES it from the live "
       "bulk IN packet size",
       "restated as a literal at %s, which is exactly how the two drifted "
       "apart last time, at a cost of 27%% on every read" % "; ".join(bad))
    ck(any(e == "bl_usb_ep2_pkt_in" and not in_irq for _n, e, in_irq in assigns),
       "and at least one of them is the plain polled boot path")

    section("the CDC set is well-formed on its own terms")
    coherent(cdc, "CDC")
    ck(len(cdc) == CFG_LEN,
       "BL_USB_CDC_CFG_LEN matches the array", "%d vs %d" % (CFG_LEN, len(cdc)))
    ifs = by_interface(cdc)
    ck(len(ifs) == 2, "two interfaces: communications and data",
       "found %d" % len(ifs))
    if len(ifs) == 2:
        comm, data = ifs[0][0], ifs[1][0]
        ck(comm[5] == 0x02 and comm[6] == 0x02,
           "interface 0 is Communications / Abstract Control Model",
           "class 0x%02X subclass 0x%02X" % (comm[5], comm[6]))
        ck(data[5] == 0x0A,
           "interface 1 is CDC Data", "class 0x%02X" % data[5])
        ck(comm[2] == 0 and data[2] == 1,
           "and they are numbered 0 and 1",
           "%d and %d" % (comm[2], data[2]))

        fn = {d[2]: d for d in ifs[0][1] if d[1] == 0x24}
        ck(set(fn) == {0x00, 0x01, 0x02, 0x06},
           "all four functional descriptors are present, and on interface 0",
           "subtypes present: %s" % sorted(fn))
        if 0x00 in fn:
            h = fn[0x00]
            ck(len(h) == 5 and (h[3] | (h[4] << 8)) == 0x0110,
               "header: bLength 5, bcdCDC 1.10",
               "len %d, bcdCDC 0x%04X" % (len(h), h[3] | (h[4] << 8)))
        if 0x01 in fn:
            c = fn[0x01]
            ck(len(c) == 5 and c[4] == data[2],
               "call management: bLength 5, bDataInterface names interface 1",
               "len %d, points at %d" % (len(c), c[4]))
        if 0x02 in fn:
            a = fn[0x02]
            ck(len(a) == 4 and (a[3] & 0x02) != 0,
               "ACM: bLength 4, bmCapabilities claims line coding + line state",
               "len %d, caps 0x%02X" % (len(a), a[3]))
        if 0x06 in fn:
            u = fn[0x06]
            ck(len(u) == 5 and u[3] == comm[2] and u[4] == data[2],
               "union: control interface 0, subordinate interface 1",
               "len %d, %d -> %d" % (len(u), u[3], u[4]))

        eps0 = [d for d in ifs[0][1] if d[1] == 0x05]
        eps1 = [d for d in ifs[1][1] if d[1] == 0x05]
        ck(len(eps0) == 1 and eps0[0][2] == 0x81 and (eps0[0][3] & 3) == 3,
           "interface 0 carries exactly the interrupt IN endpoint 0x81",
           "%s" % [("0x%02X" % e[2]) for e in eps0])
        ck(len(eps0) == 1 and (eps0[0][4] | (eps0[0][5] << 8)) == 8,
           "and it is 8 bytes, the same seven bytes the CH340 set declares")
        ck(len(eps1) == 2, "interface 1 carries exactly two endpoints",
           "found %d" % len(eps1))

    eps = endpoints(cdc)
    ck(set(eps) == {0x81, 0x82, 0x02},
       "the endpoint addresses are unchanged from the CH340 set",
       "found %s" % sorted(eps))
    for a in (0x82, 0x02):
        if a in eps:
            ck(eps[a]["attr"] == 0x02,
               "EP 0x%02X is bulk" % a, "bmAttributes 0x%02X" % eps[a]["attr"])
            ck(eps[a]["mps"] == 64,
               "EP 0x%02X wMaxPacketSize is 64" % a, "%d" % eps[a]["mps"])
    for a, e in sorted(eps.items()):
        ck(e["mps"] <= 64,
           "EP 0x%02X fits the silicon's 64-byte maximum "
           "(datasheet V2.1 s17.2.2)" % a, "%d" % e["mps"])

    # The DMA window has to hold them. Table 17-4, BUF_MOD=1: RX at +0 and +64,
    # TX at +128 and +192. Four 64-byte windows, 256 bytes, which is exactly
    # ep2_buf, so 64-byte packets fill each half and none can overrun.
    m = re.search(r"ep2_buf\[(\d+)\]", usb_src)
    ck(m is not None and int(m.group(1)) >= 256,
       "ep2_buf holds all four 64-byte double-buffer windows",
       "ep2_buf is %s bytes" % (m.group(1) if m else "?"))
    for name, want in (("EP2_DBUF_RX0_OFF", 0x00), ("EP2_DBUF_RX1_OFF", 0x40),
                       ("EP2_DBUF_TX0_OFF", 0x80), ("EP2_DBUF_TX1_OFF", 0xC0)):
        ck(const(usb_src, name) == want,
           "%s is 0x%02X, as Table 17-4 says" % (name, want),
           "found %s" % const(usb_src, name))
    ck(const(usb_src, "EP2_RX_WINDOW") == 64,
       "the receive clamp is the full 64-byte window")
    ck(const(usb_src, "RX_CREDIT_BYTES") == 64,
       "and receive credit is only granted when a whole 64-byte packet fits")

    section("the CDC identity is ours, and is not squatting")
    ck(cdc_dev[4] == 0x02 and cdc_dev[5] == 0x02,
       "bDeviceClass/bDeviceSubClass are 02/02, which is what loads usbser.sys",
       "0x%02X/0x%02X" % (cdc_dev[4], cdc_dev[5]))
    ck(cdc_dev[7] == EP0_PKT,
       "bMaxPacketSize0 still agrees with BL_USB_EP0_PKT",
       "descriptor %d, code %s" % (cdc_dev[7], EP0_PKT))
    got_vid = cdc_dev[8] | (cdc_dev[9] << 8)
    got_pid = cdc_dev[10] | (cdc_dev[11] << 8)
    ck(got_vid == VID and got_pid == PID,
       "the descriptor uses BL_USB_CDC_VID / BL_USB_CDC_PID",
       "%04X:%04X vs %04X:%04X" % (got_vid, got_pid, VID or 0, PID or 0))
    ck(got_vid != 0x1A86,
       "and it is NOT WCH's VID. A CDC device on 1A86:7523 would still be "
       "bound by CH341SER.SYS, which is the failure this change exists to avoid",
       "%04X" % got_vid)
    ck(got_vid == 0x1209,
       "it is pid.codes (0x1209), the VID that exists so open projects do not "
       "have to squat", "%04X" % got_vid)

    # FlashGBX finds hardware by scanning serial ports for a VID/PID, so the
    # hw_GBFlash.py shipped at the repo root carries a second copy of this
    # identity. When the two disagree the device enumerates cleanly, works when
    # named with --device-port, and is never discovered, with nothing logged to
    # say why.
    #
    # This used to check tools/flashgbx/gbflash_open_discovery.patch. That patch
    # is no longer how the change is delivered, and the directory is not part of
    # the repository, so a fresh clone failed here while the author's tree
    # passed. The invariant is the same one; the file carrying it moved.
    host_path = os.path.join(FW, "hw_GBFlash.py")
    ck(os.path.exists(host_path),
       "the hw_GBFlash.py that FlashGBX needs is shipped at the repo root")
    if os.path.exists(host_path):
        blob = re.sub(r"\s+", " ", open(host_path).read())
        want = "comports[i].vid == 0x%04X and comports[i].pid == 0x%04X" % (VID, PID)
        ck(want.lower() in blob.lower(),
           "and its port scan tests BL_USB_CDC_VID / BL_USB_CDC_PID",
           "usb.h says 0x%04X:0x%04X, and hw_GBFlash.py does not test for it. "
           "FlashGBX would never find the device and would not say why"
           % (VID, PID))
    ck("pid.codes" in usb_hdr and "CH341SER.SYS" in usb_hdr,
       "and usb.h says whose VID this is and why WCH's cannot be used",
       "the PID choice must stay explained. pid.codes will not allocate to a "
       "software-only project for hardware it did not open, and 1A86 is "
       "unusable because CH341SER.SYS binds it. Without that written down, "
       "the next reader files a pull request that gets rejected")
    ck(cdc_dev[2] | (cdc_dev[3] << 8) == 0x0110,
       "bcdUSB is 1.10, so no host asks for a device qualifier",
       "0x%04X" % (cdc_dev[2] | (cdc_dev[3] << 8)))
    ck(cdc_dev[14] == 0 and cdc_dev[15] == 0 and cdc_dev[16] == 0,
       "no string indices, exactly as today: GET_DESCRIPTOR type 3 STALLs")
    ck(cdc_dev[17] == 1, "one configuration")
    ck(cdc[5] != 0,
       "bConfigurationValue is not 0. FW_USB_CDC_BREAK=1 must not be on in a "
       "normal build", "bConfigurationValue %d" % cdc[5])

    section("each build serves ONE set, and nothing patches it")
    # Which set is served is a compile-time choice, so both tables are const
    # and neither is written after its initialiser. A table left writable
    # invites a runtime patch and costs RAM the device does not have.
    writes = re.findall(
        r"(?<!uint8_t )bl_usb_desc_config(?:_cdc)?\s*\[[^\]]*\]\s*=(?!=)",
        desc_src + usb_src + main_src)
    ck(not writes,
       "neither configuration descriptor is assigned after its initialiser",
       "%r" % writes)
    for name in ("bl_usb_desc_config", "bl_usb_desc_device",
                 "bl_usb_desc_config_cdc", "bl_usb_desc_device_cdc"):
        ck("const uint8_t %s[" % name in desc_src,
           "%s is const, i.e. in flash" % name)
    ck("bl_usb_desc_dev_live" not in desc_src
       and "bl_usb_desc_cfg_live" not in desc_src,
       "and usb_desc.c holds no live pointer: the names ep0_standard() serves "
       "are resolved by the preprocessor",
       "a pointer here would be a runtime indirection")

    section("the class requests EP0 has to answer")
    ck("BMREQ_CLASS_IN" in usb_src and "BMREQ_CLASS_OUT" in usb_src,
       "ep0_setup() routes 0xA1 and 0x21 as whole-byte bmRequestType matches")
    for name, code in (("CDC_SET_LINE_CODING", 0x20),
                       ("CDC_GET_LINE_CODING", 0x21),
                       ("CDC_SET_CONTROL_LINE_STATE", 0x22)):
        ck(const(usb_hdr, name) == code,
           "%s is 0x%02X" % (name, code), "found %s" % const(usb_hdr, name))
        ck(name in usb_src, "and usb.c has an arm for it")
    body = usb_src[usb_src.index("static void ep0_out(void)"):]
    body = _fn_body(body)
    ck("CDC_SET_LINE_CODING" in body,
       "ep0_out() consumes SET_LINE_CODING's OUT data stage",
       "it is the only control transfer this device answers that has one")
    ck("RB_UEP_T_TOG | UEP_T_RES_ACK" in body,
       "and arms the status stage as an IN ZLP with DATA1",
       "UEP0_CTRL_IDLE has T_RES=NAK: the host's status IN would hang")
    ck("CDC_LINE_CODING_LEN" in body,
       "and clamps the copy to the 7 bytes the structure has, whatever the "
       "host says arrived")

    section("the EP0 data stage dispatches on the request TYPE, not just the code")
    # ep0_in() must dispatch the DATA stage on bmRequestType, not on bRequest
    # alone. On the code alone the standard arms run with state nobody prepared:
    #
    #   bRequest 5 -> R8_USB_DEV_AD loaded from a raw wLength, so the device
    #                 abandons its USB address and vanishes until a port reset.
    #   bRequest 6 -> ep0_descr is stale, pointing past the descriptor it last
    #                 served, and the vendor path never clamps ep0_req_len, so
    #                 the device streams up to ~65 KB of SRAM to the host.
    #
    # 0xA1/0x06 and 0x21/0x05 are legal requests for a fuzzer to send.
    body = usb_src[usb_src.index("static void ep0_in(void)"):]
    body = body[:body.index("\nstatic ")]
    ck("ep0_req_type" in body,
       "ep0_in() consults ep0_req_type",
       "it dispatches on ep0_req_code alone. A vendor or class IN with "
       "bRequest 5 or 6 runs the standard SET_ADDRESS / GET_DESCRIPTOR arm")
    ck("REQ_SET_ADDRESS" in body and "REQ_GET_DESCRIPTOR" in body,
       "and still handles the two standard requests it is responsible for")

    section("the CH340 vendor paths are unconditional")
    # FW_USB_CDC=0 is a CH340 and needs them. Putting them behind the switch
    # builds a device that enumerates as a CH340 and stalls every request the
    # driver sends.
    ck("BMREQ_VENDOR_IN" in usb_src and "BMREQ_VENDOR_OUT" in usb_src,
       "the 0xC0 replay and the 0x40 accept-and-ZLP arms are unconditional")
    ck("CH341_REQ_SERIAL_INIT" in usb_src,
       "and so is the ch341 session marker")
    ck(re.search(r"#if\s+FW_USB_CDC\s*\n[^#]*BMREQ_VENDOR", usb_src) is None,
       "none of them is inside an #if FW_USB_CDC")


def coherent(cfg, name):
    ck(len(cfg) >= 9 and cfg[0] == 0x09 and cfg[1] == 0x02,
       "%s: it starts with a configuration descriptor header" % name)
    total = cfg[2] | (cfg[3] << 8)
    ck(total == len(cfg),
       "%s: wTotalLength matches the array length" % name,
       "says %d, array is %d" % (total, len(cfg)))
    subs = walk(cfg)
    ck(sum(len(d) for d in subs) == len(cfg),
       "%s: every sub-descriptor length adds up with nothing left over" % name)
    ck(all(d[0] >= 2 for d in subs),
       "%s: no sub-descriptor is shorter than its own header" % name)
    ifaces = [d for d in subs if d[1] == 0x04]
    ck(len(ifaces) == cfg[4],
       "%s: bNumInterfaces matches the interface descriptors present" % name,
       "says %d, found %d" % (cfg[4], len(ifaces)))
    for iface, owned in by_interface(cfg):
        got = len([e for e in owned if e[1] == 0x05])
        ck(iface[4] == got,
           "%s: interface %d's bNumEndpoints matches the endpoints that follow "
           "it" % (name, iface[2]),
           "says %d, found %d" % (iface[4], got))


def in_mps_offset(cfg):
    pos = 0
    out = None
    for d in walk(cfg):
        if d[1] == 0x05 and (d[2] & 0x80) and d[3] == 0x02:
            out = pos + 4
        pos += len(d)
    return out


if __name__ == "__main__":
    sys.exit(main())
