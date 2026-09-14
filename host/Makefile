# host/Makefile: offline suites for the GBFlash open firmware.
#
#   make -C host test
#
# Needs a C compiler and python3. No cross toolchain, no device, no cartridge.
# ../src/proto.c, ../src/cart.c and ../src/lk_glue.c are compiled for the host
# and driven through ctypes.
#
# test_stale_claims checks that source comments are true now: citations
# resolve, no path is marked ungated that STATUS.md later gated, no symbol
# is called unused while it has callers, and no stated knob default
# contradicts the Makefile. results/STATUS.md is an append-only journal, so
# its old claims are history rather than defects; check 5 only reports where
# a newer entry supersedes one, and never fails.
#
# test_host_update checks what the updater offers: an Open-GBFlash device is
# compared against res/fw_Open-GBFlash.zip, a stock one still gets upstream's
# answer. It needs the FlashGBX package and skips without it.
#
# test_late_ack reproduces the resync race that shifts a ROM dump by one byte
# and checks hw_GBFlash.py's _try_write closes it. It drives a fake port, needs
# no device, and skips without the FlashGBX package.
#
# test_rom_var_resend checks ReadROM sends TRANSFER_SIZE and DMG_ACCESS_MODE
# once rather than per call, and sends them again after anything else writes
# them. Drives a fake port, needs no device.
#
# test_host_attrs guards hw_GBFlash.py against a self.X() naming a method on
# another class in the file. GbxDevice extends LK_Device from FlashGBX, which a
# clone does not carry, so checks 3 and 4 only run where that tree is present.
#
# test_no_inert_state deliberately does not scan src/lk_glue.c: mirroring a
# field into an LK variable is not acting on it, and adding the file would let
# a one-line assignment satisfy the guard. test_lk_seam covers the mirror from
# the other end, every variable LK.c reads must be written.

PYTHON ?= python3

.PHONY: test clean

test:
	$(PYTHON) test_proto.py
	$(PYTHON) test_state_mirror.py
	$(PYTHON) test_cart.py
	$(PYTHON) test_no_inert_state.py
	$(PYTHON) test_usb_desc.py
	$(PYTHON) test_lk_seam.py
	$(PYTHON) test_lk_glue.py
	$(PYTHON) test_stale_claims.py
	$(PYTHON) test_host_attrs.py
	$(PYTHON) test_late_ack.py
	$(PYTHON) test_rom_var_resend.py
	$(PYTHON) test_host_update.py

clean:
	rm -rf build __pycache__
