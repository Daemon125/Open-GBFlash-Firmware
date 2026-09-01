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

clean:
	rm -rf build __pycache__
