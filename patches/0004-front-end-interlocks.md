carried patch 0004: the 5 V interlock and the power-on settle
==================================================================

**This one is not a diff.** The other three carried
patches are unified diffs against `LK.c` because they change statements inside
function bodies. These two changes cannot be expressed as a diff to `LK.c`
either, but for a different reason: they are not changes to what a command
*does*, they are constraints on *whether and when a command may run at all*.
That is above the command layer, so it lives in the framing front end,
`src/lk_glue.c` and `src/main.c`, and this file is its documentation.

Both are front-end work rather than `LK.c` work: see the table in
`patches/README.md`.

---

## 1. The 5 V interlock (mismatch M2)

### What the guard is

`fw_cart_voltage()` (`src/cart.c:156-176`) refuses to raise the cartridge rail
to 5 V unless the device is positively in DMG mode. `include/cart.h:75-83`:

> 5 V at a 3.3 V AGB cartridge is the one irreversible mistake here, which is
> why `fw_cart_voltage()` refuses it outside DMG mode rather than trusting the
> host to ask correctly.

`host/test_cart.py` pins it. Deleting `&& dmg_mode` from `cart.c` left the
entire rest of the suite green, which is why that test exists.

### Why upstream defeats it, by construction

`LK.c:190-194`:

```c
case LK_CMD_SET_VOLTAGE_5V:
    _lk_var8[LK_VAR8_CART_MODE] = LK_MODE_DMG;
    SET_VOLTAGE_5V();
```

The mode variable is force-set to DMG **on the line before** the macro runs.
Any guard reachable from `SET_VOLTAGE_5V()` that consults
`_lk_var8[LK_VAR8_CART_MODE]` therefore always passes. The interlock would go
inert while continuing to look present, which is worse than not having one,
because it would still be listed as implemented by
`tools/lk_upstream_diff.py`, still be covered by a test that passes, and still
be described in the header.

There is no fix inside `LK_device.h`. Everything a board macro can see has
already been overwritten by the time it is called.

### Where it went instead

The front end sees the opcode byte before `lk_loop()` ever does, and our own
parser records the mode at that moment: `proto.c:710-713` sets
`st->mode = FW_MODE_DMG` when it sees `0xA3`, and `proto.c:702-704` sets
`FW_MODE_AGB` on `0xA2`. Nothing below the parser writes `st->mode`. In
particular, `fw_lk_dispatch()`'s state mirror does **not** copy
`_lk_var8[LK_VAR8_CART_MODE]` back into it (`src/lk_glue.c`, `mirror_from_lk`).

So `fw_lk_voltage_5v()` in `src/lk_glue.c` calls the *unchanged*
`fw_cart_voltage(1, st->mode == FW_MODE_DMG)`. The guard itself never moved:
it is still the same function, still tested by the same assertion in
`host/test_cart.py`. What moved is the *evidence*: from LK's variable, which
upstream overwrites, to ours, which it cannot reach.

The host's ordering makes this correct rather than merely safe:
`LK_Device.py:960-961` sends `SET_MODE_DMG` and then `SET_VOLTAGE_5V`, in that
order, so a legitimate 5 V request is never refused.

### Status in stage 1

`0xA5` is **not routed**, so `SET_VOLTAGE_5V()` is never reached: our own
handler in `main.c` already calls `fw_cart_voltage()`. The macro is implemented
correctly anyway, because "the macro exists but is wrong" is exactly the silent
failure this port exists to avoid, and because routing `0xA5` later must not
require somebody rediscovering `LK.c:190`.

### What must never be done

Routing `0xA5` to `lk_loop()` **without** this. And adding
`_lk_var8[LK_VAR8_CART_MODE]` to `mirror_from_lk()`, which would look like
tidying up an asymmetric mirror and would silently re-defeat the interlock.
There is a comment saying so at both ends.

---

## 2. The power-on settle and its 100/200 split (mismatch M3d)

### What the wait is

`FW_CART_POWERON_MS = 300` (`include/cart.h:45`). Measured on a
ChisFlash AGB flash cart, power-cycled between trials and asked for its header
eight times, logo SHA-1 against a known answer:

```
0.30 s  XXXXXXXX        0.60 s  X.......
0.40 s  XXXXXXXX        0.75 s  ........
0.50 s  XX......         1.2 s  ........ x40, no failures
```

Below the threshold every read came back shifted forward by one halfword,
stable, self-consistent and wrong.

`LK.c:905` ends `lk_cart_power_on()` with `_delay_ms(150)`.

### Why it cannot simply be lengthened

The host waits 1 s for *any* command's ACK
(`LK_Device.py:132`). 1000 ms of settle inside the handler, plus a 471 ms
`CALC_CRC32` as the first thing that follows, blows that timeout, and the
host's response to a timeout is to power-cycle the cartridge and retry, which
starts the settle again and times out again. That loop was a real failure.

So 100 ms is spent inside the `CART_PWR_ON` handler, before its ACK, and the
remaining 200 ms is charged to whatever touches the cartridge first
(`cart.h:142-166`, `cart_wait_ready()` in `main.c`).

`lk_cart_power_on()` is straight-line and blocking. A deferred deadline cannot
live inside it: the function has already returned before the deferred half
would be checked. `CART_POWER_ON()` is the wrong hook too: it is one store
near the *start* of the sequence, ~150 us before the end.

### Where it went instead

`fw_lk_dispatch()` (`src/lk_glue.c`) calls `fw_cart_wait_ready()` before
`lk_loop()`. `main.c` still owns the deadline, because `main.c` owns the
`CART_PWR_ON` handler that sets it.

The notes worried that this forces the front end to "enumerate which opcodes
touch the cartridge", a new coupling in the wrong direction. In stage 1 that
enumeration is free and needs no separate list: **every routed opcode touches
the cartridge**, without exception, because the routed set is defined as "DMG
bus commands that do work". The routing table *is* the enumeration. If an
opcode is ever added to `fw_lk_routes()` that does not touch the cartridge,
the result is a wasted wait, not a bug, which is the safe direction.

`fw_cart_wait_ready()` pumps `bl_usb_poll()` while it waits, so the link stays
alive through it.

### Status in stage 1

Live. Every routed command pays it, and after the first cartridge access it
costs nothing at all (the deadline is in the past).

### What must never be done

Adding an opcode to `fw_lk_routes()` and *removing* the `fw_cart_wait_ready()`
call to save the compare. It is one compare against a timestamp that is almost
always already expired.

---

## Why these two are listed as "carried patches" at all

Because they are exactly the same kind of debt as the three diffs: a measured
fix of ours that upstream does not have, which has to be re-checked on every
upstream bump, and which nothing in the build can prove is still correct. The
three diffs at least fail loudly when their anchors move. **These two do not.**

`tools/lk_upstream_diff.py` will happily report a green port whose interlock is
inert. The only guard is a human reading `LK.c`'s `SET_VOLTAGE_5V` case and
`lk_cart_power_on()` after each bump. That is written into the upstream-bump
checklist in `patches/README.md`, and it is the weakest link in this port.
