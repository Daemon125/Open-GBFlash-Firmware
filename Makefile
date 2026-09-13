# GBFlash open firmware.
#
# build/fw.bin is the installable image: the payload in the LFBG container the
# bootloader validates. build/firmware.bin is the bare payload, with no
# boot-info record, and the bootloader refuses it.
#
# If an image does not boot, hold U22 at power-on to force update mode and
# send another. The jumper is only for replacing the bootloader itself.

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
NM      := $(CROSS)nm
PYTHON  ?= python3

BUILD   := build
TARGET  := firmware
LDSCRIPT := ld/firmware.ld


ASRC := src/vectors.S src/start.S
CSRC := src/main.c src/proto.c src/cart.c src/usb.c src/usb_desc.c src/timebase.c \
        src/lk_glue.c

OBJ := $(addprefix $(BUILD)/,$(notdir $(ASRC:.S=.o)) $(notdir $(CSRC:.c=.o))) \
       $(BUILD)/LK.o

APP_BUDGET := 231424         # 0x38800, the official bootloader's cap

# upstream/FlashGBX_LK_Firmware/{LK.c,LK.h} must stay byte-identical to
# upstream; tools/lk_upstream_diff.py fails if they differ. Fixes go to a copy
# under build/lk/, one patch file each. See patches/README.md.
#
# LK_PATCHES feeds both the apply list and the -D list: LK_device_ch579.h
# #errors without FW_LK_PATCH_0001/2/3, so a patch that stops applying is a
# build failure rather than a silently different device.
#
# LK_DIR must be assigned before CFLAGS, a `:=` assignment using -I$(LK_DIR):
# defined later it expands to nothing and the build fails on a missing LK.h.
LK_DIR := upstream/FlashGBX_LK_Firmware

LK_PATCHES := patches/0001-agb-save-flash-readback-poll.patch \
              patches/0002-agb-address-latch-settle.patch \
              patches/0003-dmg-flash-we-pin-fallback.patch

LK_PATCH_IDS  := $(foreach p,$(LK_PATCHES),$(firstword $(subst -, ,$(notdir $(p)))))
LK_PATCH_DEFS := $(addprefix -DFW_LK_PATCH_,$(LK_PATCH_IDS))

LK_BUILD := $(BUILD)/lk

ARCHFLAGS := -mcpu=cortex-m0 -mthumb

# Below 1730592000 FlashGBX flags the firmware as unofficial and offers to
# overwrite it on every connect. include/fw_config.h:14 #errors on it.
FW_TIMESTAMP ?= 1788313704

# Must be 0. With echo on, the loopback pump drains the receive staging before
# the protocol framer sees a byte: the device enumerates, mirrors what is sent
# to it and answers no command at all, BOOTLOADER_RESET included, so it cannot
# be told over USB to accept a replacement. Recovery is U22 held at power-on.
BL_USB_ECHO ?= 0

# Must reach the assembler as well as the compiler: src/vectors.S points vector
# 22 (IRQ6 = USB) at the handler from this symbol and src/usb.c enables that
# interrupt from the same one. A skewed pair sends the first USB interrupt to
# fw_fault_handler, which spins: a dead board needing U22 at power-on.
# EXTRA_CFLAGS reaches CFLAGS only, so `make EXTRA_CFLAGS=-DFW_USB_IRQ=1`
# produces exactly that skew. bl_usb_irq_arm() re-checks __vectors[22] at
# runtime, so a skewed build is slow rather than dead.
#
# [MEASURED] us/byte Single/MemCpy/Stream, byte-verified over 4 MiB: polled
# 3.738 / 2.305 / 1.284, 640.0 KiB/s; irq 3.875 / 2.330 / 1.230, 672.1 KiB/s.
FW_USB_IRQ ?= 1

# 1 gives the device its own CDC-ACM identity (VID 0x1209, PID 0x0008, see
# include/usb.h) with 64-byte bulk endpoints. 0 serves the stock CH340
# descriptor set at 32, which unmodified FlashGBX finds without a patch.
#
# 0x1209:0x0008 is pid.codes' shared test PID: it identifies no single project,
# so a host cannot tell two devices using it apart. FlashGBX matches it
# explicitly in hw_GBFlash.py.
#
# 0 must still produce an image byte-identical to the tree without this switch;
# every line it adds is inside `#if FW_USB_CDC`. Checked with md5.

FW_USB_CDC ?= 1

# Breaks the CDC descriptor set on purpose, to see what a host does with
# one it cannot use. Only meaningful with FW_USB_CDC=1. Never ship either: a
# broken image answers no command over USB, so getting off one needs U22 held
# at power-on. usb_desc.c #warnings on both.
#   1  bConfigurationValue 0: bl_usb_configured() never goes true
#   2  bulk IN declared at 0x83, which does not exist: EP2 IN never completes
FW_USB_CDC_BREAK ?= 0

# Four transport-only switches: nothing here touches the cartridge bus, so the
# failure mode is a corrupt or duplicated packet and tools/verify_agb_methods.py
# catches it. [MEASURED] one at a time, fitted us/byte Single/MemCpy/Stream
# against a 4.563 / 2.837 / 1.632 baseline:
#
#   FW_FAST_CLOCK  4.468 / 2.742 / 1.581   no-progress deadlines armed lazily,
#                                          174 cycles of bl_time_ms() per packet
#   FW_FAST_COPY   4.431 / 2.715 / 1.496   usb_copy() 16 bytes/pass, LDM/STM
#   FW_TX_REWIND   4.564 / 2.838 / 1.632   nothing to three decimals, hence off
#   FW_TX_DIRECT   4.350 / 2.582 / 1.397   g_reply handed to the endpoint, one
#                                          of three writes per byte deleted
#                                          (measured on top of CLOCK+COPY)
FW_READ_FAST ?= 1
FW_FAST_CLOCK ?= $(FW_READ_FAST)
FW_FAST_COPY  ?= $(FW_READ_FAST)
FW_TX_REWIND  ?= 0
FW_TX_DIRECT  ?= $(FW_READ_FAST)

# FW_AGB_FAST_BURST replaces the /RD strobe loop, which is the cartridge bus.
# The Stream sample point is the vendor firmware's own, /RD low + M + 7. The
# two pulse widths are ours and shorter: at the shipped FW_AGB_RD_HOLD_NOPS=0 /
# FW_AGB_RD_HI_NOPS=0 (src/cart.c) /RD low is 11 cycles and /RD high 9, against
# stock's 17 and 10 at the M = 2 assumed for a peripheral access. hold=6 / hi=1
# restores stock's widths exactly. Full table above agb_burst_fast().
#
# The failure mode is silent: a dump shifted forward by one halfword, stable
# across passes. tools/verify_agb_methods.py compares the three read methods
# against each other and a bus change moves all three alike, so the gate is
# tools/dump_agb.py --compare against a dump taken with the previous build,
# same cartridge, same slot.
# GATED ON HARDWARE: 3 x 4 MiB byte-identical to a cross-build reference
# (md5 77c48b97), all three methods agreeing, +6.6% on a Stream dump.
FW_AGB_FAST_BURST ?= 1

# FW_TX_PIPELINE stages both EP2 transmit windows so bl_usb_poll() releases the
# bus before it copies rather than after, taking the 327 measured cycles of
# RB_UIF_TRANSFER hold-off (38% of the 854-cycle dead wire per packet) off the
# critical path. Worth nothing without FW_USB_IRQ. Established on hardware:
# T_RES is still ACK after a completed IN (63/63 samples), and RB_UC_INT_BUSY
# allows at most one transaction per flag-clear, so the SIE cannot run into an
# unrefilled window.
#
# [MEASURED] macOS, every figure byte-identical to a cross-build reference,
# KiB/s off / on: 4096 bytes per command 755.5 / 847.8, 8192 777.5 / 876.0.
# Fitted Stream slope 1.230 -> 1.081 us/byte. The per-command intercept rises
# 174 -> 268 us, so it loses on small reads: tools/dump_agb.py, which re-sends
# two SET_VARIABLEs per chunk, reports it as slower.
FW_TX_PIPELINE ?= 1

# Receive mirror of FW_TX_PIPELINE: clear RB_UIF_TRANSFER before the 64-byte
# copy out of the OUT window, so the SIE can fill EP2's second receive window
# while the CPU drains the first. RB_UEP2_BUF_MOD is already set in
# usb_device_init (CH579 datasheet V2.1 p.87 Table 17-4: RX at UEP2_DMA+0
# and +64); RB_UC_INT_BUSY auto-NAKs until that flag is cleared, which is what
# makes the second window reachable at all.
#
# It re-enters the "USB receive lost one word per transfer" corruption and has
# no runtime escape: compile-time #if only, bl_usb_ep2_dbuf is never cleared.
# FW_RX_DBUF=0 must stay byte-identical to the tree without the switch.
# NOT GATED on Windows, where host-side bulk OUT pacing differs:
# tools/test_short_packet.py and a byte-exact ROM write there before the
# default moves off 0.
FW_RX_DBUF ?= 0

# 3D Memory (GBA Video) mapper settle, in iterations of 27 nops. Stock spins
# 0x190 = 10800 cycles = 337 us once per 4096-byte page, 5.5 s of a 134.8 s
# 64 MiB dump. Gate for lowering it: a 64 MiB dump byte-identical to the vendor
# reference.
FW_M3D_SETTLE_ITERS ?= 0x100

# Consume four bytes per CRC table pass instead of one. CALC_CRC32 is 24.7
# cycles/byte, ~14.5 of that in the byte-at-a-time table loop; slice-by-4 takes
# that loop to 8.75, so 19.45 overall. A 16 MiB FlashGBX ROM write CRCs 48 MiB,
# 38.8 s of a measured 182.91 s. Costs 3 KB of .rodata, no SRAM, nothing on the
# cartridge bus.
# GATED ON HARDWARE: tools/test_crc32.py 6/6 against the host's own zlib over
# real cartridge data at five sizes up to 4 MiB. 1267.5 -> 1646.4 KiB/s.
FW_CRC32_SLICE4 ?= 1

# Program the buffered-write DATA phase with stock's own inlined loop instead
# of 16 calls to fw_cart_agb_write(), which is the command-write routine. Stock
# inlines the data loop at 0x7794-0x780C with both direction writes hoisted to
# 0x7752-0x775A: 86 counted cycles per halfword against our 108, so this
# matches stock's timing rather than going below it. Worth 21 cycles/halfword
# x 16 x 524288 buffers = 5.5 s of a measured 182.91 s. Interval table above
# fw_cart_agb_write_burst() in src/cart.c.
# GATED ON HARDWARE: seven byte-exact bench_flash_write runs and five full
# 16 MiB FlashGBX writes verified. +4.8%.
# Skip programming a buffer chunk that is entirely 0xFFFF. NOR programming only
# clears bits, so it writes nothing; the host only skips whole all-0xFF
# 1024-byte blocks and pays a full unlock, burst, confirm and buffer-program
# wait for every all-FF 64-byte chunk inside a mixed block.
#
# MEASURED WORTHLESS on real GBA ROMs, which is why it ships off. Counted over
# the two 16 MiB images here: FireRed programs 144064 chunks of which 13 are
# all-FF, Emerald 233280 of which 5. The host's block skip already takes every
# FF run that matters, so this is 7.5 ms of a 46 s write. Kept because it costs
# nothing when off and does pay on an image padded inside its blocks.
FW_AGB_SKIP_FF ?= 0

FW_AGB_WRITE_BURST ?= 1

# Route the AGB save read through the nominated direct region (FW_TX_DIRECT)
# instead of the staging ring. Removes the second copy of every save byte and
# the mid-command CDC zero-length packets: the ring arms a ZLP whenever it
# empties after a full packet, and the save bus (1.97 us/byte) is slower than
# the wire (~1.2), so it empties inside every 512-byte chunk, 256 transfer
# terminators in a 128 KiB backup. No cartridge code, no bus intervals.
# GATED ON HARDWARE: four 128 KiB save backups byte-identical to the =0 build.
FW_SAVE_TX_DIRECT ?= 1

# Overlap the 3D Memory bus with its wire. It is the last read path that strobes
# a whole 4096-byte page off the cartridge before the endpoint sees a byte; the
# other four nominate g_reply and publish as they fill. Bus is 0.500 us/byte
# against 1.083 on the wire, so the margin is 2.2x, unlike the DMG grain sweep
# that FW_DMG_READ_PUB banked as negative at 1.08x.
# 0 stages a 4096-byte page through tx_buf and needs BL_USB_TX_BUF_SIZE >=
# 4096; src/main.c:691 #errors on it.
FW_M3D_TX_DIRECT ?= 1

# Word-wise payload copy in fw_proto_feed_bulk(). The FLASH_PROGRAM block is
# serialised, not overlapped: bench_flash_write measures 11.57 ms per 2048-byte
# block, the sum of 4.81 ms of transport and 6.73 ms of flash, so receive
# cycles land on the block instead of hiding in it. The byte loop this replaces
# emitted 13 cycles/byte. No cartridge code, no bus intervals.
# GATED ON HARDWARE: byte-exact bench_flash_write and full 16 MiB verified
# writes. +2.0%.
FW_PROTO_FAST_COPY ?= 1

# Sample the flash status again as soon as bl_usb_poll() returns instead of
# once per ~281-cycle loop iteration, halving the mean overshoot on each of the
# 524288 buffered programs in a 16 MiB write. Read-only on the bus.
# GATED ON HARDWARE: byte-exact bench_flash_write. +0.6%.
FW_STATUS_WAIT_TIGHT ?= 1

# Program a DMG flash byte the way stock's FLASH write does it, not the way its
# MAPPER write does. dmg_write_raw() transcribes sub_80B4, the MBC/mapper
# write; stock's FLASH_METHOD 1 driver calls sub_8790 instead: no CLK pulse, no
# PA_OUT park, eight nops of /WR rather than eleven, and no data-bus teardown.
# 84 cycles against sub_80B4's 120 and our 148. This reproduces sub_8790
# instruction for instruction, so no interval falls below stock's, and it
# repairs an address-to-data window that was shorter than stock's. Worth 336
# cycles/byte on a path measured at 42.14 us/byte against stock's ~28.
# GATED ON HARDWARE: 2 MiB and 256 KiB writes byte-exact on two 0%-blank ROMs;
# hw_suite --mode dmg 4/4 vs a vendor reference. 109.81 -> 79.48 s at 2 MiB.
FW_DMG_WRITE_BURST ?= 1

# Hoist the bus turnaround out of dmg_status_wait's loop. Both firmwares wait
# the same time for the chip; one of our iterations is 261 cycles (it pays
# fw_cart_dmg_read's whole per-call prologue to read one byte) against stock
# sub_8DD4's 51, so the mean overshoot is ~4 us per byte against stock's ~0.8.
# The /RD low -> sample window keeps its 8 nops: that one is a chip access.
# GATED ON HARDWARE: with the burst, 16.62 -> 13.28 s at 256 KiB against
# stock's 14.22, and 79.48 s at 2 MiB against stock's 80.90.
FW_DMG_POLL_TIGHT ?= 1

# Do not issue a program cycle for a 0xFF data byte. NOR programming only
# clears bits, so writing 0xFF leaves the byte as it was, whatever it holds and
# whether or not the sector was erased first. FlashGBX already drops all-0xFF
# blocks host-side; this catches the 0xFF bytes inside mixed blocks, 5.9% of
# what is actually sent for a real 2 MiB Game Boy ROM.
FW_DMG_SKIP_FF ?= 1

# Use the lean write primitive for an AMD write-buffer load, as
# FW_DMG_WRITE_BURST already does for a single byte. A 32-byte load is 37 bus
# writes; through dmg_write_raw they cost more than the chip spends programming.
# Needs FW_DMG_WRITE_BURST: the lean primitive and g_dmg_we_is_wr() are inside it.
# The part measured here reports CFI buffer_write_time_avg 128 us against the
# 419 us a load actually took.
# GATED ON HARDWARE: 8 MiB MBC3+RTC cart, S29GL-class, 2 MiB write byte-exact
# 3/3 with it and 3/3 without. 35.37 -> 30.52 s mean of three, ranges 0.08 and
# 0.65 s wide. Only the buffered path: a cart on single writes is unaffected.
FW_DMG_BUF_BURST ?= 1

# AMD unlock bypass on the DMG single-write path: enter once per payload and a
# program becomes A0 then the byte, dropping the two unlock writes from every
# byte in the run. The chip must be taken out of it before anything else,
# including erase; the exit covers the error break too.
# Needs FW_DMG_WRITE_BURST for the lean primitive.
# GATED ON HARDWARE: 8 MiB MBC3+RTC cart, S29GL-class, single-write path forced
# through a profile carrying no buffer_write. 2 MiB 76.63 -> 67.63 s, 11.7%, ROM
# byte-exact every run, then four save round trips alternating two images after a
# bypass write, 4/4 matching. That save test is what caught the AGB attempt.
# A chip without bypass ignores the A0 and its bytes fail verification, so the
# failure mode is a refused write rather than silent corruption.
FW_DMG_UNLOCK_BYPASS ?= 1

# Cycle counters around the buffered load and its status wait, read back through
# opcode 0xDF. Measurement only; never ship it.
# Hold PB_OUT in a register across one buffered load instead of read-modify-
# writing it three times per bus write. Port B is driven only from cart.c and no
# interrupt path touches GPIO. NOT GATED ON HARDWARE.
# Program a DMG buffer load as it arrives instead of waiting for the whole
# payload, the counterpart of the AGB pump. Receive and programming are
# otherwise strictly serialised. The gain is small because the CPU cannot drain
# USB and drive the cartridge at once, so the two add rather than overlap.
# GATED ON HARDWARE: 8 MiB MBC3+RTC cart, 2 MiB 30.92 -> 30.28 s mean of two,
# byte-exact, then four save round trips alternating two images, 4/4 matching.
FW_DMG_WRITE_STREAM ?= 1

FW_DMG_SHADOW_PB ?= 0

FW_DMG_PROFILE ?= 0

# Restore the live write-enable selector from the var-state blob. It must be
# flash_we_pin_var, the field main.c applies through we_pin_requested, not the
# dead flash_we_pin: 26 shipped DMG profiles select AUDIO, and on those a
# restore onto the dead field ACKs every flash write and lands nowhere.
# 0 must stay byte-identical to the tree without it.
# NOT GATED ON HARDWARE: needs a cartridge with an AUDIO-WE profile and a
# GetVarState/SetVarState cycle around a re-plug.
FW_VARSTATE_WE_PIN ?= 1

# Give the A15 read strobe back the width stock gives it. In the A15 and
# SlowA15 read variants A15 is the only strobe: /RD is dropped once in the
# prologue and A15 going high terminates the cycle. Our loop keeps the address
# counter in registers where stock reloads it from RAM between those two edges,
# so the A15-high recovery came out at 9 cycles against stock's 12, below stock
# on the strobe the cartridge acts on. Three nops, and they are FREE: measured
# 1.3650 us/byte with the pad against 1.3678 without.
# GATED ON HARDWARE: tools/verify_dmg_methods.py byte-exact over 2 MiB against
# a vendor reference. A15 is the default DMG read method, so this is every GUI
# DMG dump.
FW_DMG_A15_PAD ?= 1

# Put dmg_write_raw()'s four sub-stock intervals back. It is no longer the hot
# flash-write path (FW_DMG_WRITE_BURST is) but it is still every MBC bank
# select, every RAM-enable and every DMG save byte. Counted against stock
# sub_80B4:
#
#   PB_DIR|=0xFF -> PB_CLR=0xFF      4 vs 5     cosmetic
#   data valid -> /CS low            7 vs 10    real: /CS gates the write cycle
#   /WR low -> CLK low               2 vs 5     cosmetic
#   PB_CLR=0xFF -> PB_DIR&=~0xFF     5 vs 10    cosmetic
#
# The /CS one is a reorder, not a pad: stock asserts /CS after its 3-nop pad
# (0x818A then 0x8198). The other three cost 9 cycles on a ~176-cycle routine.
# GATED ON HARDWARE: ChisFlash S29GL256 cart, 512 KiB ROM and 32 KiB save both
# byte-exact over three round trips plus 60 s unpowered retention. Costs
# nothing measurable: save write 1.36 s against 1.37 s unpadded.
FW_DMG_WRITE_RAW_PAD ?= 1

# Give the /CS-pulse read cycle back its access time. Stock releases D0..D7
# inside the /RD-low window (0x7FC0, six cycles before it samples at 0x7FC8);
# this code hoists that release into the prologue, which is free everywhere
# except here, where both edges are the cartridge's. Counted:
#
#   /CS low -> /RD low      3 vs 6      /RD low -> sample   14 vs 22
#   /CS low -> sample      20 vs 28     address -> sample   25 vs 34
#
# Also +1 nop on the plain /RD variant, whose address -> /RD-low setup is 7
# against stock 0x7F26..0x7F30's 8. Costs 9 cycles on a 56-cycle CS-pulse loop.
# GATED ON HARDWARE, in two halves. The plain /RD pad:
# tools/verify_dmg_methods.py byte-exact over 512 KiB against a vendor L15
# reference. The /CS-pulse pair: FlashGBX sets DMG_READ_CS_PULSE=1 for every
# RAM read, so the DMG save round trips gate it, 32 KiB byte-identical vendor
# against ours and 100 soak cycles with no drift. No measurable cost.
FW_DMG_CS_READ_PAD ?= 1

# Set the system clock here instead of inheriting it from the bootloader.
# Assembly-only, so it must reach ASFLAGS; no C source reads it. src/start.S
# carries the reasoning.
#
# The A/B is not a throughput test: Fsys scales the cartridge bus delays and
# every device-side loop. Measure a device-bound span, CALC_CRC32 over a fixed
# region, which repeats within 0.05% and would move by a whole factor if the
# clock had changed.
FW_INIT_CLOCK ?= 1

# R16_CLK_SYS_CFG low byte. 0x88 = 32 MHz from the internal RC, stock's choice.
# 0x4C = internal RC into the 480 MHz PLL divided by 12, 40 MHz, SetSysClock
# case 13 in the vendor image. No crystal either way; bit 9 would need one and
# retail boards lack it.
#
# It reaches ASFLAGS for start.S AND drives FW_FSYS_HZ below, which reaches
# CFLAGS. It is not assembly-only: bl_time_ms converts SysTick cycles to
# microseconds with it.
#
# SHIPS AT 40 MHz, gated on all three cartridges: AGB and DMG reads byte-exact
# on every read method at full size against vendor-firmware dumps, a 64 MiB 3D
# Memory read byte-exact, ROM writes byte-exact on both platforms, and save
# restore/backup cycles alternating differing images.
#
# The CART_PWR_ON ack is a wall-clock interval and does not move with Fsys; it
# cannot show the clock changed.
# FW_BUS_NOP_NUM/DEN must move with this: see below.
FW_SYS_CLK_CFG ?= 0x4C
# Powers the PLL in start.S before the clock switch. The shipped 0x4C is a PLL
# mode and still leaves this 0: the bootloader hands over with
# R8_HFCK_PWR_CTRL already 0x1C, so the PLL is up before we run.
FW_SYS_CLK_PLL ?= 0

# Fsys implied by FW_SYS_CLK_CFG, for the C side. FW_SYS_CLK_CFG reached only
# ASFLAGS, so bl_time_ms kept converting cycles to microseconds at 32 MHz while
# the part ran at 40 and every millisecond deadline expired at 80% of its
# intended wall-clock time. Change this with the clock or the build fails.
ifeq ($(FW_SYS_CLK_CFG),0x4C)
FW_FSYS_HZ ?= 40000000
else
FW_FSYS_HZ ?= 32000000
endif

# Cartridge intervals are counted in CPU cycles, so raising Fsys shortens every
# one of them. Scale them back by NUM/DEN, rounded up. This pads the NOP RUN
# only: fixed instruction cycles inside the same window are not scaled, so a
# window still shortens by up to (N-n)/N of its length. It is not a guarantee
# that no strobe ever gets shorter. 5/4 goes with FW_SYS_CLK_CFG=0x4C;
# use 1/1 with 0x88. The AGB leaf declares its intervals in CYCLES with the
# fixed instruction cost subtracted, so BUS_SCALE is applied to the cycle total
# and the nop counts follow; its own assembly-time check re-counts that.
# GATED ON AGB HARDWARE 2026-08-30: all three read methods byte-exact over a
# full 16 MiB against a vendor reference, and a 16 MiB write verified.
FW_BUS_NOP_NUM ?= 5
FW_BUS_NOP_DEN ?= 4

# Diagnostic opcode 0xEE: write R16_CLK_SYS_CFG and read it plus
# R8_HFCK_PWR_CTRL back. Payload 0xFFFF reads without writing.
FW_CLKPROBE ?= 0


# Route DMG_CART_READ (0xB1) to the LK port. LK's reader sends through
# lk_conn_send(), which does not use the nominated direct region, so routing
# 0xB1 forfeits FW_TX_DIRECT and FW_TX_PIPELINE.
# GATED ON HARDWARE: tools/verify_dmg_methods.py 3/3 against a vendor reference
# over 2 MiB. Off is 4.85 -> 4.03 s.
FW_LK_ROUTE_DMG_READ ?= 0

# Send a DMG cartridge read from the nominated direct region instead of copying
# it through the transmit staging ring, as FW_SAVE_TX_DIRECT does for AGB
# saves. No effect when 0xB1 is routed to LK, which sends through its own path.
# GATED ON HARDWARE: tools/verify_dmg_methods.py against a vendor reference.
# 4.03 -> 3.83 s.
FW_DMG_TX_DIRECT ?= 1

# Stop draining each chunk to the wire before reading the next one. With
# FW_USB_IRQ the handler sends published bytes on its own, so the cartridge
# read and the USB transfer overlap. A 2 MiB DMG dump measured 3.83 s, the sum
# of a ~2.16 s bus and a ~1.69 s wire rather than the max of them.
# GATED ON HARDWARE: 3.83 -> 3.80 s.
FW_DMG_TX_OVERLAP ?= 1

# Publish into the transmit region every FW_DMG_PUB_GRAIN bytes during the
# cartridge read rather than once per chunk at the end of it.
# KiB/s at 40 MHz: off 945.5 / 946.5; on at grain 64 934.9, 128 952.7,
# 256 955.1 / 952.0, 512 946.6, 1024 948.8. Ships grain 256.
FW_DMG_READ_PUB ?= 1

FW_DMG_PUB_GRAIN ?= 256

# Implement 0xAB/0xAC the way stock does. stock_L15_full.bin 0x9828:
# GPIOA_ModeCfg(0xFFFF, IN_PU), GPIOB_ModeCfg(0xFF, IN_PU), 5.000 ms, then
# var8[0x0E]=1; 0xAC @ 0x9880 is the same with mode 0. WCH mode 1 is
# `PD_DRV &= ~m ; PU |= m ; DIR &= ~m`, so the command also floats the bus.
# On AGB the data word arrives on PA, so stock's PB pull-ups are inert.
# GATED ON HARDWARE: 0xAC is unconditional at fw_ver>=8, so the float plus 5 ms
# runs on every dump, and those came back byte-identical to a vendor reference.
# Still open: a dump of an M29W640 or S29GL256 AGB flashcart, the two profiles
# that actually set "enable_pullups".
FW_CART_PULLUPS_FULL ?= 1

# The read path and the receive path want different optimisation levels.
# [MEASURED] echo transport (FW_ECHO_PAYLOAD, 2048-byte blocks), and fitted
# us/byte Single / MemCpy / Stream:
#
#     all -Os                     395.1 kB/s
#     all -O2                     356.2        3.875 / 2.330 / 1.081
#     all -O3                     357.7
#     usb.o+proto.o -Os, rest -O2 400.3        3.844 / 2.314 / 1.085
#
# The echo bench repeats to about 3.5%. Re-measure the receive path whenever
# an optimisation flag moves.
FW_RX_OPT ?= -Os

# Decouple the USB arming rate from the latch rate. tx_pump() arms at most one
# packet per poll (there is a single R8_UEP2_T_LEN) and Stream polls once per
# 128-byte latch group: two packets of data, one arming opportunity. This polls
# inside the group without deselecting, so the group stays 128.
#
# It re-enters the configuration measured to corrupt Single and MemCpy (the
# pump running while /CS is asserted), though only for groups of at least two
# packets, which those two methods never produce. Off until gated on a
# cross-build byte comparison of several MiB. src/main.c also #errors on
# FW_AGB_LEAF && FW_STREAM_MIDGROUP_POLL, so gating it means FW_AGB_LEAF=0 too.
FW_STREAM_MIDGROUP_POLL ?= 0

# Raise the AUDIO latch before arming PB20 as the flash write-enable.
# fw_cart_set_we_pin(AUDIO) sets direction only, and fw_cart_audio_drive(0)
# leaves the level at 0: arm after that and /WE stays low, every dmg_write_raw
# pulse is inverted, and the AMD unlock is issued with the address changing
# while /WE is asserted. Reached by an AGB session then a DMG AUDIO-WE flash
# with no read in between (26 of 98 fc_DMG_*.txt profiles select AUDIO).
#
# No bus interval moves: the added store is to PB_OUT while PB20 is still an
# input. =0 exists as a byte-identical bisection point.
# NOT GATED ON HARDWARE: needs one byte-verified ROM write on an AUDIO-WE
# profile after reproducing that sequence, AGB mode first.
FW_CART_AUDIO_WE_SAFE ?= 1

# Collapse the AGB read into one naked leaf. In the three-call path
# (fw_cart_agb_open / _burst / _close per latch group) the call frames ARE the
# address-latch settle: delete them without paying the interval back and the
# dump shifts forward by one halfword, identically on all three read methods,
# so tools/verify_agb_methods.py cannot see it.
#
# Counted pads, shipped value and the three-call value each replaces:
#
#     FW_AGB_LEAF_ADDR_CYCLES     13   AD valid -> /CS low       (three-call 17)
#     FW_AGB_LEAF_SETTLE_CYCLES   20   /CS low  -> first /RD low (three-call 72)
#     FW_AGB_LEAF_CSHI_CYCLES     36   /CS high -> next /CS low  (three-call 86)
#     FW_AGB_LEAF_RDHI_CYCLES     12   last /RD high -> /CS high (three-call 49)
#
# CSHI 36 is below stock's 80. Revert it first if a cartridge reads shifted;
# the three-call column is the connector-identical rollback. The leaf is worth
# nothing itself; it stops call overhead flooring the intervals. Sweeps in
# src/cart.c, gate order in results/STATUS.md.
FW_AGB_LEAF ?= 1

# Do not re-arm PB20 (cart pin 31, AGB /IRQ, DMG audio-in) as a push-pull
# output on every cartridge power-on. fw_cart_init() leaves it an input
# because a cartridge may drive it; fw_cart_power(1)'s blanket
# `PB_DIR |= 0xC7 << 13` re-arms it anyway, discarding the AGB_IRQ_ENABLED=0
# the host sends just before CART_PWR_ON. Arms only on AGB_IRQ_ENABLED,
# DMG_AUDIO_ENABLED or FLASH_WE_PIN==AUDIO, which drives the line less than
# either stock (sub_4EA0 0x4EC4/0x4ED4) or LK (LK.c:891-895).
# If a cartridge misbehaves on pin 31, revert this first.
# NOT FULLY GATED: an AGB ROM dump byte-identical to the =0 build and one ROM
# write on a "write_pin":"AUDIO" DMG profile are not among what ran
# (results/STATUS.md, 2026-08-20).
FW_CART_AUDIO_HONOUR ?= 1

# -fno-builtin and -fno-tree-loop-distribute-patterns are required: without
# them GCC rewrites hand-written byte loops into calls to memcpy/memset, which
# do not exist under -nostdlib.
#
# -fno-jump-tables is for speed, not linkability. GCC 15 turns a dense switch
# into a Thumb-1 dispatch table calling __gnu_thumb1_case_sqi; libgcc is linked
# (see LDLIBS) so those would link, but they cost a call per dispatch on the
# protocol hot path, and proto.c and lk_loop() are both one big switch.
#
# [MEASURED] us/byte Single/MemCpy/Stream: -Os 4.265 / 2.496 / 1.297,
# -O2 4.215 / 2.454 / 1.290, -O3 4.468 / 2.587 / 1.297.
CFLAGS := $(ARCHFLAGS) \
          -O2 \
          -ffreestanding -fno-builtin -fno-common \
          -fno-tree-loop-distribute-patterns \
          -fno-jump-tables \
          -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -ffunction-sections -fdata-sections \
          -Wall -Wextra \
          -DFW_TIMESTAMP=$(FW_TIMESTAMP) \
          -DBL_USB_ECHO=$(BL_USB_ECHO) \
          -DFW_USB_IRQ=$(FW_USB_IRQ) \
          -DFW_USB_CDC=$(FW_USB_CDC) \
          -DFW_USB_CDC_BREAK=$(FW_USB_CDC_BREAK) \
          -DFW_BUS_NOP_NUM=$(FW_BUS_NOP_NUM) \
          -DFW_BUS_NOP_DEN=$(FW_BUS_NOP_DEN) \
          -DFW_CLKPROBE=$(FW_CLKPROBE) \
          -DFW_CART_PULLUPS_FULL=$(FW_CART_PULLUPS_FULL) \
          -DFW_FAST_CLOCK=$(FW_FAST_CLOCK) \
          -DFW_FAST_COPY=$(FW_FAST_COPY) \
          -DFW_TX_REWIND=$(FW_TX_REWIND) \
          -DFW_TX_DIRECT=$(FW_TX_DIRECT) \
          -DFW_AGB_FAST_BURST=$(FW_AGB_FAST_BURST) \
          -DFW_TX_PIPELINE=$(FW_TX_PIPELINE) \
          -DFW_RX_DBUF=$(FW_RX_DBUF) \
          -DFW_M3D_SETTLE_ITERS=$(FW_M3D_SETTLE_ITERS) \
          -DFW_CRC32_SLICE4=$(FW_CRC32_SLICE4) \
          -DFW_PROTO_FAST_COPY=$(FW_PROTO_FAST_COPY) \
          -DFW_VARSTATE_WE_PIN=$(FW_VARSTATE_WE_PIN) \
          -DFW_STATUS_WAIT_TIGHT=$(FW_STATUS_WAIT_TIGHT) \
          -DFW_CART_AUDIO_WE_SAFE=$(FW_CART_AUDIO_WE_SAFE) \
          -DFW_DMG_WRITE_BURST=$(FW_DMG_WRITE_BURST) \
          -DFW_DMG_POLL_TIGHT=$(FW_DMG_POLL_TIGHT) \
          -DFW_DMG_SKIP_FF=$(FW_DMG_SKIP_FF) \
          -DFW_DMG_BUF_BURST=$(FW_DMG_BUF_BURST) \
          -DFW_DMG_UNLOCK_BYPASS=$(FW_DMG_UNLOCK_BYPASS) \
          -DFW_DMG_PROFILE=$(FW_DMG_PROFILE) \
          -DFW_DMG_SHADOW_PB=$(FW_DMG_SHADOW_PB) \
          -DFW_DMG_WRITE_STREAM=$(FW_DMG_WRITE_STREAM) \
          -DFW_DMG_A15_PAD=$(FW_DMG_A15_PAD) \
          -DFW_DMG_WRITE_RAW_PAD=$(FW_DMG_WRITE_RAW_PAD) \
          -DFW_DMG_CS_READ_PAD=$(FW_DMG_CS_READ_PAD) \
          -DFW_LK_ROUTE_DMG_READ=$(FW_LK_ROUTE_DMG_READ) \
          -DFW_DMG_TX_DIRECT=$(FW_DMG_TX_DIRECT) \
          -DFW_DMG_TX_OVERLAP=$(FW_DMG_TX_OVERLAP) \
          -DFW_DMG_READ_PUB=$(FW_DMG_READ_PUB) \
          -DFW_DMG_PUB_GRAIN=$(FW_DMG_PUB_GRAIN) \
          -DFW_AGB_SKIP_FF=$(FW_AGB_SKIP_FF) \
          -DFW_AGB_WRITE_BURST=$(FW_AGB_WRITE_BURST) \
          -DFW_SAVE_TX_DIRECT=$(FW_SAVE_TX_DIRECT) \
          -DFW_M3D_TX_DIRECT=$(FW_M3D_TX_DIRECT) \
          -DFW_STREAM_MIDGROUP_POLL=$(FW_STREAM_MIDGROUP_POLL) \
          -DFW_AGB_LEAF=$(FW_AGB_LEAF) \
          -DFW_CART_AUDIO_HONOUR=$(FW_CART_AUDIO_HONOUR) \
          $(LK_PATCH_DEFS) \
          -DLK_DEVICE_HEADER='"LK_device_ch579.h"' \
          -DBL_FSYS_HZ=$(FW_FSYS_HZ) -DBL_TIME_FSYS_HZ=$(FW_FSYS_HZ) \
          -Iinclude -I$(LK_DIR) -MMD -MP $(EXTRA_CFLAGS)

ASFLAGS := $(ARCHFLAGS) -x assembler-with-cpp \
           -DFW_USB_IRQ=$(FW_USB_IRQ) \
           -DFW_INIT_CLOCK=$(FW_INIT_CLOCK) \
           -DFW_SYS_CLK_CFG=$(FW_SYS_CLK_CFG) \
           -DFW_SYS_CLK_PLL=$(FW_SYS_CLK_PLL) \
           -Iinclude -MMD -MP

# Every `?=' knob must be passed as -D, or named here. The two lists are
# maintained by hand; a dropped -D silently ships the in-source fallback.
# FW_FSYS_HZ reaches the compiler as BL_FSYS_HZ and BL_TIME_FSYS_HZ.
FW_KNOBS_NO_D := CROSS PYTHON FW_READ_FAST FW_RX_OPT FW_FSYS_HZ
FW_MK := $(firstword $(MAKEFILE_LIST))
FW_KNOBS_ORPHAN := $(filter-out $(FW_KNOBS_NO_D) \
  $(shell sed -n 's/.*-D\([A-Z][A-Z0-9_]*\)=.*/\1/p' "$(FW_MK)"), \
  $(shell sed -n 's/^\([A-Z][A-Z0-9_]*\)[[:space:]]*?=.*/\1/p' "$(FW_MK)"))
ifneq ($(FW_KNOBS_ORPHAN),)
$(error declared but never passed as -D: $(FW_KNOBS_ORPHAN))
endif

LDFLAGS := $(ARCHFLAGS) -nostdlib -nostartfiles \
           -T $(LDSCRIPT) \
           -Wl,--gc-sections \
           -Wl,--no-warn-rwx-segments \
           -Wl,-Map=$(BUILD)/$(TARGET).map \
           -Wl,--print-memory-usage

# The link needs libgcc: LK.c divides u16 by u16 in ten places (LK.c:382, :605,
# :618, :625, :632, :641, :654, :662, :675, :1745) and Cortex-M0 has no divide
# instruction, so GCC emits __aeabi_uidiv. -nostdlib suppresses libgcc as well
# as libc, hence the explicit -lgcc after the objects.
LDLIBS := -lgcc

Q := $(if $(V),,@)



# Without this, adding a target above `all:` silently changes what `make` does.
.DEFAULT_GOAL := all

ifeq ($(wildcard $(LK_DIR)/LK.c),)
$(warning )
$(warning $(LK_DIR) is empty. It is a git submodule.)
$(warning Run:  git submodule update --init --recursive)
$(warning )
endif

# Nothing depends on lk-check: a plain `make` does not run it. Run it after a
# submodule update, before trusting the build.
lk-check:
	$(Q)$(PYTHON) tools/lk_upstream_diff.py

.PHONY: all size disasm host-test clean lk-check knobcheck dist

# Builds every boolean knob flipped from its default. A knob that reaches
# CFLAGS can still ship one unbuildable arm, which is how FW_DMG_WRITE_BURST=0
# stayed broken. Needs the cross toolchain, so it is not in host-test.
knobcheck:
	$(PYTHON) tools/check_knob_builds.py

all: $(BUILD)/fw.bin

# The artifact FlashGBX installs. Both member names are fixed by the updater:
# fw.bin and fw.ini at the top level, nothing else. fw_ver must match
# FW_VERSION or FlashGBX offers the update again on every launch.
#
# Named fw_Open-GBFlash.zip because it goes BESIDE the vendor's fw_GBFlash.zip
# rather than over it. The chooser in hw_GBFlash.py looks for this name.
DIST     := dist
# No '#' in this awk program: make would take it as a comment and never find
# the closing paren.
FW_VER_N := $(shell awk '/define FW_VERSION/ {print $$3; exit}' include/fw_config.h)

dist: $(BUILD)/fw.bin
	$(Q)mkdir -p $(DIST)
	$(Q)cp $(BUILD)/fw.bin $(DIST)/fw.bin
	$(Q)printf '[Firmware]\nfw_ver = L%s\nfw_buildts = %s\n' \
	    '$(FW_VER_N)' '$(FW_TIMESTAMP)' > $(DIST)/fw.ini
	$(Q)cd $(DIST) && rm -f fw_Open-GBFlash.zip && zip -q -X fw_Open-GBFlash.zip fw.bin fw.ini
	$(Q)echo "  DIST    $(DIST)/fw_Open-GBFlash.zip"

$(BUILD):
	$(Q)mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.S | $(BUILD)
	$(Q)echo "  AS      $<" && $(CC) $(ASFLAGS) -c -o $@ $<

# make compares timestamps, not compiler flags: without this, a rebuild after
# changing EXTRA_CFLAGS leaves the previous binary in place and the A/B
# measures one build twice. The comparison must happen at parse time; a stamp
# the objects depend on runs too late. $(shell cat) not $(file <...): macOS
# ships GNU make 3.81. FW_INIT_CLOCK and FW_RX_OPT are not in global $(CFLAGS).
CFLAGS_STAMP := $(BUILD)/.cflags
FLAG_STAMP := $(CFLAGS) $(ASFLAGS) $(FW_RX_OPT)
ifneq ($(strip $(shell [ -f $(CFLAGS_STAMP) ] && cat $(CFLAGS_STAMP))),$(strip $(FLAG_STAMP)))
$(shell mkdir -p $(BUILD) && rm -f $(BUILD)/*.o $(BUILD)/*.d && printf '%s' '$(FLAG_STAMP)' > $(CFLAGS_STAMP))
endif

$(BUILD)/usb.o:   CFLAGS += $(FW_RX_OPT)
$(BUILD)/proto.o: CFLAGS += $(FW_RX_OPT)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(Q)echo "  CC      $<" && $(CC) $(CFLAGS) -c -o $@ $<

# --fuzz=0 is required: GNU patch's default --fuzz=2 applies a hunk with two
# context lines mismatched, silently, which was reproduced on patches/0003.
#
# Keep every path here quoted. This project lives in a directory whose name
# contains a space, and unquoted $(CURDIR)/$$p expands to two words: `patch -i`
# reads the first and reports "patch file '/Users/.../Claude/Code' not found".
$(LK_BUILD)/LK.c: $(LK_DIR)/LK.c $(LK_PATCHES) | $(BUILD)
	$(Q)mkdir -p "$(LK_BUILD)"
	$(Q)cp "$<" "$@"
	$(Q)for p in $(LK_PATCHES); do \
	  echo "  PATCH   $$p"; \
	  patch --forward --batch --fuzz=0 -p1 -d "$(LK_BUILD)" -i "$(CURDIR)/$$p" >/dev/null || { \
	    echo "  FAIL    $$p did not apply to $(LK_DIR)/LK.c"; \
	    echo "          Upstream has moved under a carried fix. Read patches/README.md;"; \
	    echo "          re-derive with patches/make_patches.py. DO NOT skip the patch."; \
	    rm -f "$@" "$(LK_BUILD)/LK.c.orig" "$(LK_BUILD)/LK.c.rej"; exit 1; }; \
	done
	$(Q)rm -f "$(LK_BUILD)/LK.c.orig"

# LK.c is compiled with this tree's CFLAGS, warnings included. The two warnings
# it emits are upstream's, at LK.c:1838 and :2129 (`data_buffer[x] != 0xFFFF`
# on a u8 array, so the skip-already-erased-words optimisation never fires).
$(BUILD)/LK.o: $(LK_BUILD)/LK.c | $(BUILD)
	$(Q)echo "  CC      $< (vendored + $(words $(LK_PATCHES)) carried patches)" && \
	  $(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/$(TARGET).elf: $(OBJ) $(LDSCRIPT)
	$(Q)echo "  LD      $@" && $(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)
	@# No libc, no crt0, no runtime: an undefined symbol here means GCC
	@# emitted a call to something that does not exist, and the link would
	@# otherwise succeed with a null relocation.
	$(Q)u=$$($(NM) -u $@); \
	if [ -n "$$u" ]; then \
	  echo "  FAIL    undefined symbols in $@:"; echo "$$u" | sed 's/^/          /'; \
	  rm -f $@; exit 1; \
	fi

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(Q)echo "  OBJCOPY $@" && $(OBJCOPY) -O binary $< $@
	$(Q)sz=$$(wc -c < $@); \
	if [ "$$sz" -gt $(APP_BUDGET) ]; then \
	  echo "  FAIL    $@ is $$sz bytes, over the $(APP_BUDGET) budget"; \
	  rm -f $@; exit 1; \
	fi; \
	echo "  PAYLOAD $$sz bytes / $(APP_BUDGET) budget ($$(( $(APP_BUDGET) - $$sz )) free)"

# mkimage.py runs the official bootloader's acceptance gates (the stricter of
# the two, see APP_BUDGET) and refuses to write a file the device would reject.
$(BUILD)/fw.bin: $(BUILD)/$(TARGET).bin
	$(Q)$(PYTHON) tools/mkimage.py $< -o $@ --quiet


size: $(BUILD)/$(TARGET).elf
	$(Q)$(SIZE) $<
	@echo
	@echo "  per-module .text:"
	$(Q)for o in $(OBJ); do \
	  printf "    %-24s %6s\n" "$$(basename $$o)" \
	    "$$($(SIZE) $$o | awk 'NR==2{print $$1}')"; \
	done

disasm: $(BUILD)/$(TARGET).elf
	$(Q)$(OBJDUMP) -d -S -z $< > $(BUILD)/$(TARGET).lst
	@echo "  DISASM  $(BUILD)/$(TARGET).lst"

host-test:
	$(Q)$(MAKE) -C host test

clean:
	$(Q)rm -rf $(BUILD)

-include $(OBJ:.o=.d)
