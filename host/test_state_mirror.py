#!/usr/bin/env python3
"""Check test_proto.py's ctypes mirror of fw_state_t field by field.

test_proto's own guards, sizeof and the last field's offset, both miss an
insertion that lands in existing interior padding: the struct does not grow,
and every field after it silently reads its neighbour's bytes.
"""

import ctypes
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def header_fields():
    """Field names of fw_state_t, in declaration order, as the host build sees it.

    Read from the preprocessed header: fw_state_t has members inside
    configuration conditionals, and offsetof on a field the build excluded is a
    compile error. The flags here must match build_lib()'s in test_proto.py,
    which builds the library whose layout is being checked.
    """
    with tempfile.TemporaryDirectory() as td:
        stub = os.path.join(td, "stub.c")
        open(stub, "w").write('#include "proto.h"\n')
        r = subprocess.run([os.environ.get("CC", "cc"), "-E", "-std=c99",
                            "-I", os.path.join(ROOT, "include"), stub],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("test_state_mirror: could not preprocess proto.h\n"
                     + r.stderr)
        pre = r.stdout

    end = pre.index("} fw_state_t;")
    body = pre[pre.rindex("typedef struct", 0, end):end]
    out, seen = [], set()
    # Any type, not just the uint*_t ones: fw_state_t has `fw_mode_t mode'.
    pat = r'^\s*(?:struct\s+|union\s+|enum\s+)?[A-Za-z_]\w*\s+(\w+)\s*(?:\[[^\]]*\])?\s*;'
    for name in re.findall(pat, body, re.M):
        if name not in seen:
            seen.add(name)
            out.append(name)
    return out


def compiler_offsets(fields):
    src = ['#include <stdio.h>', '#include <stddef.h>', '#include "proto.h"',
           'int main(void){']
    src.append('  printf("sizeof %zu\\n", sizeof(fw_state_t));')
    for f in fields:
        src.append('  printf("%s %%zu\\n", offsetof(fw_state_t, %s));' % (f, f))
    src += ['  return 0;', '}']

    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "off.c")
        exe = os.path.join(td, "off")
        open(c, "w").write("\n".join(src))
        r = subprocess.run(["cc", "-I", os.path.join(ROOT, "include"),
                            "-I", HERE, "-o", exe, c],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("test_state_mirror: could not build the offset probe\n"
                     + r.stderr)
        out = subprocess.run([exe], capture_output=True, text=True).stdout

    got = {}
    for line in out.splitlines():
        k, v = line.split()
        got[k] = int(v)
    return got


def main():
    sys.path.insert(0, HERE)
    import test_proto

    fields = header_fields()
    real = compiler_offsets(fields)

    state = test_proto.Proto.State
    mirror = {n: getattr(state, n).offset for n, _t in state._fields_}

    fails = 0

    for f in fields:
        if f not in mirror:
            print("  [FAIL] %-24s in fw_state_t at %d, absent from the mirror"
                  % (f, real[f]))
            fails += 1

    for f in mirror:
        if f not in real:
            print("  [FAIL] %-24s in the mirror, not in fw_state_t" % f)
            fails += 1

    checked = 0
    for f in fields:
        if f in mirror:
            checked += 1
            if mirror[f] != real[f]:
                print("  [FAIL] %-24s C says %d, mirror says %d"
                      % (f, real[f], mirror[f]))
                fails += 1

    if ctypes.sizeof(state) != real["sizeof"]:
        print("  [FAIL] sizeof: C says %d, mirror says %d"
              % (real["sizeof"], ctypes.sizeof(state)))
        fails += 1

    print("test_state_mirror: %d fields, %d offsets checked, %d failures"
          % (len(fields), checked, fails))
    if fails:
        print("\n  Update Proto.State in host/test_proto.py. Field ORDER there"
              "\n  must match include/proto.h exactly. ctypes lays the mirror"
              "\n  out itself and only agrees with the compiler if the"
              "\n  declaration order is the same.")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
