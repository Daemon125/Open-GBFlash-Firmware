#!/usr/bin/env python3
"""Fail the build when hw_GBFlash.py calls a method its class cannot reach.

A self.X() naming a method defined on a different class in the same file raises
AttributeError only on the line that runs it. _ResyncStalled shipped that way:
defined on FirmwareUpdater, called from GbxDevice, behind an `and` that
short-circuited until a device stopped answering.

Both checks are self-contained. GbxDevice extends LK_Device from the FlashGBX
package, which a clone does not carry, so an unknown name is only a failure when
this file defines it somewhere else, or when the class has no external base.
Check 3 upgrades to the full membership test when a FlashGBX tree is present.
"""

import ast
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TARGET = os.path.join(ROOT, "hw_GBFlash.py")

# Bases outside this file, and where to look for their sources.
FLASHGBX_DIRS = (
    os.path.abspath(os.path.join(ROOT, "..", "..", "FlashGBX-stock", "FlashGBX")),
    os.path.abspath(os.path.join(ROOT, "..", "..", "FlashGBX", "FlashGBX")),
)

# Names this file defines on one class that an external base also defines, so a
# call on another class is reaching the base, not the local one. Check 4 fails a
# entry that the base does not actually have.
INHERITED = {
    "TryConnect": "LK_Device.TryConnect(port, baudrate); FirmwareUpdater has an "
                  "unrelated one-argument method of the same name",
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


def classes(tree):
    # FirmwareUpdaterWindow sits inside a try: guarding the Qt import.
    return {n.name: n for n in ast.walk(tree) if isinstance(n, ast.ClassDef)}


def defined_on(cls):
    """Names reachable through self: methods, class attrs, self.X assignments."""
    names = set()
    for node in ast.walk(cls):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            names.add(node.name)
        elif isinstance(node, ast.Assign):
            for t in node.targets:
                if isinstance(t, ast.Name):
                    names.add(t.id)
                elif (isinstance(t, ast.Attribute)
                      and isinstance(t.value, ast.Name) and t.value.id == "self"):
                    names.add(t.attr)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            names.add(node.target.id)
    return names


def self_calls(cls):
    """(name, lineno) for every direct self.NAME(...) inside the class."""
    out = []
    for node in ast.walk(cls):
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                and isinstance(node.func.value, ast.Name)
                and node.func.value.id == "self"):
            out.append((node.func.attr, node.lineno))
    return out


def base_names(cls):
    return [ast.unparse(b) for b in cls.bases]


def find_module(name):
    for d in FLASHGBX_DIRS:
        p = os.path.join(d, name + ".py")
        if os.path.exists(p):
            return p
    return None


def external_members(base):
    """Names on an external base, or None when its source is not here."""
    mod = base.split(".")[0]
    path = find_module(mod)
    if path is None:
        return None
    try:
        tree = ast.parse(io.open(path, encoding="utf-8").read())
    except (OSError, SyntaxError):
        return None
    cs = classes(tree)
    if base not in cs:
        return None
    names = defined_on(cs[base])
    for b in base_names(cs[base]):
        more = external_members(b)
        if more:
            names |= more
    return names


def check_misplaced(cs):
    print("\n1. a method defined in this file is called on a class that has it")
    owners = {}
    for name, cls in cs.items():
        for n in defined_on(cls):
            owners.setdefault(n, set()).add(name)

    for cname in sorted(cs):
        own = defined_on(cs[cname])
        bad = []
        for called, line in sorted(set(self_calls(cs[cname]))):
            if called in own:
                continue
            if called not in owners:
                continue  # comes from an external base
            if called in INHERITED:
                continue
            bad.append("hw_GBFlash.py:%d  self.%s() but %s is defined on %s"
                       % (line, called, called, ", ".join(sorted(owners[called]))))
        ck(not bad, "%s reaches every in-file method it calls" % cname,
           "\n".join(bad))


def check_selfcontained(cs):
    print("\n2. a class with no external base resolves every self call")
    checked = 0
    for cname in sorted(cs):
        bases = base_names(cs[cname])
        if [b for b in bases if b != "object"]:
            continue
        checked += 1
        own = defined_on(cs[cname])
        bad = ["hw_GBFlash.py:%d  self.%s() is not defined on %s"
               % (line, called, cname)
               for called, line in sorted(set(self_calls(cs[cname])))
               if called not in own]
        ck(not bad, "%s resolves every self call" % cname, "\n".join(bad))
    if checked == 0:
        print("     no class without an external base")


def check_external(cs):
    print("\n3. a class with an external base resolves every self call")
    for cname in sorted(cs):
        bases = [b for b in base_names(cs[cname]) if b != "object"]
        if not bases:
            continue
        names = set(defined_on(cs[cname]))
        unresolved = []
        for b in bases:
            more = external_members(b)
            if more is None:
                unresolved.append(b)
            else:
                names |= more
        if unresolved:
            print("     %s skipped, no source here for %s"
                  % (cname, ", ".join(unresolved)))
            continue
        bad = ["hw_GBFlash.py:%d  self.%s() is on neither %s nor %s"
               % (line, called, cname, "/".join(bases))
               for called, line in sorted(set(self_calls(cs[cname])))
               if called not in names]
        ck(not bad, "%s resolves every self call" % cname, "\n".join(bad))


def check_exemptions(cs):
    print("\n4. every INHERITED name is really on an external base")
    bases = set()
    for cls in cs.values():
        bases |= {b for b in base_names(cls) if b != "object"}
    resolved = {}
    for b in sorted(bases):
        more = external_members(b)
        if more is not None:
            resolved[b] = more
    if not resolved:
        print("     no external base source here; exemptions unverified")
        return
    for name, why in sorted(INHERITED.items()):
        on = [b for b, names in resolved.items() if name in names]
        ck(bool(on), "%s is on %s" % (name, "/".join(sorted(resolved))),
           "" if on else "no external base defines it; the exemption is stale "
                         "and hides a real call\n  reason given: %s" % why)


def main():
    print("test_host_attrs: hw_GBFlash.py must not call a method off its class")
    if not os.path.exists(TARGET):
        print("  [FAIL] %s is missing" % TARGET)
        return 1
    tree = ast.parse(io.open(TARGET, encoding="utf-8").read())
    cs = classes(tree)
    print("  %d classes: %s" % (len(cs), ", ".join(sorted(cs))))
    check_misplaced(cs)
    check_selfcontained(cs)
    check_external(cs)
    check_exemptions(cs)
    print("\ntest_host_attrs: %d passed, %d failed" % (_pass, _fail))
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
