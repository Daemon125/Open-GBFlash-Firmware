#!/usr/bin/env python3
"""Fail the build for any fw_state_t field the host can set that nothing reads.

A field stored and never read looks implemented and is not. Three shipped that
way; each produced output the host reported as success, and each needed a
cartridge in the slot to catch. A field with nothing to act on yet goes in
INERT_BY_DESIGN with its reason.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

INERT_BY_DESIGN = {
    "status_register":       "an OUTPUT, not a selector: the device writes it on a "
                             "program timeout (agb_status_wait) and the host reads "
                             "it back to report what the chip was saying. The "
                             "host-set direction is write-only by design",
    "dmg_access_mode":       "the address window (0xA000+) and the CS-pulse flags "
                             "already carry this; kept so GET_VARIABLE answers",
    "flash_we_pin":          "SET_FLASH_CMD's own copy; the handler also writes "
                             "flash_we_pin_var, which is what main.c applies. "
                             "The var-state blob now carries the LIVE cell in "
                             "both directions (FW_VARSTATE_WE_PIN), so this "
                             "field is a mirror kept only for struct stability",
    "cart_mode":             "the LIVE bus mode is fw_state_t::mode, and only "
                             "SET_MODE_DMG/AGB (0xA2/0xA3) set it. That is on purpose: "
                             "the 5 V interlock takes its evidence from the "
                             "OPCODE, never from a host-writable variable (see "
                             "fw_lk_voltage_5v in lk_glue.c). cart_mode is the "
                             "GET_VARIABLE and var-state copy of what the host "
                             "asked for. NOTE THAT THIS MAKES SET_VARIABLE("
                             "CART_MODE) a no-op on the bus, which upstream's "
                             "single _lk_var8 cell is not. Open question, not "
                             "a settled decision",
}


def main():
    hdr = open(os.path.join(ROOT, "include", "proto.h")).read()
    body = hdr[hdr.index("typedef struct"):hdr.index("} fw_state_t;")]
    fields = sorted(set(re.findall(r'^\s+(?:uint\d+_t|int)\s+(\w+)', body, re.M)))

    # get_variable() and the var-state blob only shuffle values around; a field
    # appearing solely there is still inert. Strip from proto.c only: on the
    # concatenation a missing end marker would swallow main.c.
    def drop_fn(s, start):
        """Remove one function by brace matching.

        The end marker used to be a "/* ---" banner, which broke as soon as the
        banner was deleted."""
        while start in s:
            i = s.index(start)
            b = s.index("{", i)
            depth = 0
            for j in range(b, len(s)):
                if s[j] == "{":
                    depth += 1
                elif s[j] == "}":
                    depth -= 1
                    if depth == 0:
                        s = s[:i] + s[j + 1:]
                        break
            else:
                break
        return s
    proto = open(os.path.join(ROOT, "src", "proto.c")).read()
    proto = drop_fn(proto, "static uint32_t get_variable")
    proto = drop_fn(proto, "static uint32_t get_var_state")
    proto = drop_fn(proto, "static uint32_t set_var_state")
    src = proto + open(os.path.join(ROOT, "src", "main.c")).read()

    # A comment is not a reader: `uses` is a bare word-boundary count over the
    # source, so a comment naming a field pushes uses above assigns. Strip
    # after the drops above, which use "/* ------" as their end marker.
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)

    inert = []
    for f in fields:
        uses = len(re.findall(r'\b%s\b' % f, src))
        assigns = len(re.findall(r'st->%s\s*=(?!=)' % f, src))
        if uses <= assigns:
            inert.append(f)

    unexplained = [f for f in inert if f not in INERT_BY_DESIGN]
    stale = [f for f in INERT_BY_DESIGN if f not in inert and f in fields]

    for f in unexplained:
        print("  FAIL %s is set by the host and nothing reads it" % f)
    for f in stale:
        print("  FAIL %s now has a reader; drop it from INERT_BY_DESIGN" % f)

    n = len(fields)
    print("test_no_inert_state: %d fields, %d acted on, %d declared inert, "
          "%d failures" % (n, n - len(inert), len(INERT_BY_DESIGN),
                           len(unexplained) + len(stale)))
    return 1 if (unexplained or stale) else 0


if __name__ == "__main__":
    sys.exit(main())
