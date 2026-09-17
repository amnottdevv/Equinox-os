# ============================================================
#  Makefile - Equinox OS Kernel
#  Target: i686-elf (32-bit)
# ------------------------------------------------------------
#  Toolchain auto-detect (priority order):
#    1. i686-elf-g++.exe   (Windows cross toolchain)
#    2. i686-elf-g++       (Linux cross toolchain)
#    3. host g++ -m32      (needs gcc-multilib + i386 libgcc;
#                           LIBGCC32_DIR may point to the 32-bit
#                           libgcc.a — see HOST Mode)
# ============================================================

# ---- Toolchain detection ----
ifeq ($(shell command -v i686-elf-g++.exe 2>/dev/null),)
  ifeq ($(shell command -v i686-elf-g++ 2>/dev/null),)
    # HOST MODE: use the host g++ with -m32 (every flag below is
    # already -m32). The 32-bit libgcc.a is required at link time
    # (-lgcc). With the gcc-multilib package installed, -lgcc is
    # found automatically; if the i386 libgcc was extracted
    # manually, point LIBGCC32_DIR at its folder, e.g.:
    #   make LIBGCC32_DIR=$HOME/tools/root/usr/lib/gcc/i686-linux-gnu/14
    CC      := g++
    CC_C    := gcc
    LD      := g++
    OBJCOPY := objcopy
    HOSTCC  := 1
    LIBGCC32_DIR ?= $(HOME)/tools/root/usr/lib/gcc/i686-linux-gnu/14
    LIBGCC_LDFLAGS := -L$(LIBGCC32_DIR) -no-pie
    PIE_CFLAGS     := -fno-pie
  else
    CC      := i686-elf-g++
    CC_C    := i686-elf-gcc
    LD      := i686-elf-g++
    OBJCOPY := i686-elf-objcopy
    HOSTCC  := 0
    LIBGCC_LDFLAGS :=
    PIE_CFLAGS     :=
  endif
else
  CC      := i686-elf-g++.exe
  CC_C    := i686-elf-gcc.exe   # C compiler for LVGL .c files
  LD      := i686-elf-g++.exe
  OBJCOPY := i686-elf-objcopy.exe
  HOSTCC  := 0
  LIBGCC_LDFLAGS :=
  PIE_CFLAGS     :=
endif

GRUB    := grub-mkrescue
AS      := nasm

# ------------------------------------------------------------
#  GRUB module directory. grub-mkrescue looks for the platform in
#  /usr/lib/grub (the compile-time path). If it is not on the
#  system (e.g. a locally extracted toolchain), point -d at the
#  i386-pc folder explicitly — without this, grub-mkrescue builds
#  an ISO WITHOUT a boot image, silently (boot failed: could not
#  read from CDROM).
# ------------------------------------------------------------
GRUB_PC_DIR := $(firstword $(wildcard /usr/lib/grub/i386-pc $(HOME)/tools/root/usr/lib/grub/i386-pc))
ifneq ($(GRUB_PC_DIR),)
  GRUB := grub-mkrescue -d $(GRUB_PC_DIR)
endif

# Exported for mrp_pack.py (avoid the .exe variants)
export MRP_CXX = $(CC)
export MRP_OBJCOPY = $(OBJCOPY)

# Directories
ROOT_DIR = $(CURDIR)
BOOT_DIR = $(ROOT_DIR)/boot
KERNEL_DIR = $(ROOT_DIR)/kernel
BUILD_DIR = $(ROOT_DIR)/build
DIST_DIR  = $(ROOT_DIR)/dist
ISO_DIR   = $(ROOT_DIR)/iso_root
TEST_DIR  = $(ROOT_DIR)/test

# Source files (ALL .cpp recursively under kernel/)
# NOTE: kernel/library/UI/filemanager.cpp (the MorphTUI/CLI file
#       manager, OLD) is excluded because
#       kernel/gui/app/filemanager/filemanager.cpp (the LVGL GUI
#       file manager, NEW) already provides `filemanager_open()`
#       with an identical signature.
CPP_SRC = $(shell find $(KERNEL_DIR) -type f -name "*.cpp" -not -path "$(KERNEL_DIR)/library/UI/filemanager.cpp")
C_SRC   = $(shell find $(KERNEL_DIR) -type f -name "*.c" -not -path "*/gui/lvgl/*" -not -path "*/library/lvgl/*")

# LVGL (only if the source tree exists at kernel/gui/lvgl/)
LVGL_DIR = $(KERNEL_DIR)/gui/lvgl
ifneq ($(wildcard $(LVGL_DIR)/src/lvgl.h),)
  # Exclude vendor-specific GPU folders — Equinox OS only uses SW renderer.
  LVGL_C_SRC  = $(shell find $(LVGL_DIR)/src -type f -name "*.c" \
                -not -path "*/draw/renesas/*" \
                -not -path "*/draw/nxp/*" \
                -not -path "*/draw/stm32_dma2d/*" \
                -not -path "*/draw/arm2d/*" \
                -not -path "*/draw/sdl/*" \
                -not -path "*/draw/swm341_dma2d/*")
  LVGL_C_OBJ  = $(patsubst $(LVGL_DIR)/src/%.c,$(BUILD_DIR)/lvgl/%.o,$(LVGL_C_SRC))
  LVGL_FLAG   = -DHAS_LVGL
else
  LVGL_C_OBJ  =
  LVGL_FLAG   =
endif

# ------------------------------------------------------------
# lwIP 2.1.3 (v10.11) — TCP/IP stack, NO_SYS=1 (cooperative
# polling). Source: third_party/lwip-2.1.3 (BSD-3). Only core
# + ipv4 + netif/ethernet.c; tcpip.c is skipped (needs threading).
# v10.13: dns.c IS built (LWIP_DNS=1 — hostname resolution via
# 10.0.2.3, works without threading through the NO_SYS callback).
# ------------------------------------------------------------
LWIP_DIR = $(ROOT_DIR)/third_party/lwip-2.1.3/src
ifneq ($(wildcard $(LWIP_DIR)/include/lwip/init.h),)
  LWIP_CORE_SRC = $(filter-out %/tcpip.c,$(wildcard $(LWIP_DIR)/core/*.c))
  LWIP_IPV4_SRC = $(filter-out %/igmp.c %/autoip.c,$(wildcard $(LWIP_DIR)/core/ipv4/*.c))
  LWIP_NETIF_SRC = $(LWIP_DIR)/netif/ethernet.c
  LWIP_C_SRC = $(LWIP_CORE_SRC) $(LWIP_IPV4_SRC) $(LWIP_NETIF_SRC)
  LWIP_C_OBJ = $(patsubst $(LWIP_DIR)/%.c,$(BUILD_DIR)/lwip/%.o,$(LWIP_C_SRC))
else
  LWIP_C_OBJ =
endif

# Object files (maintain directory structure under build/)
ASM_OBJ = $(BUILD_DIR)/boot/start.o
CPP_OBJ = $(patsubst $(KERNEL_DIR)/%.cpp,$(BUILD_DIR)/kernel/%.o,$(CPP_SRC))
C_OBJ   = $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/kernel/%.o,$(C_SRC)) $(LVGL_C_OBJ)
OBJS    = $(ASM_OBJ) $(CPP_OBJ) $(C_OBJ) $(LWIP_C_OBJ)

# Linker script
LINKER_SCRIPT = $(ROOT_DIR)/linker.ld

# Compiler flags
COMMON_FLAGS   = -m32 -ffreestanding -fno-exceptions -fno-rtti -Wall -Wextra -O2 -g $(PIE_CFLAGS)
COMMON_FLAGS_C = -m32 -ffreestanding -Wall -Wextra -O2 -g $(PIE_CFLAGS)
INCLUDES     = -I$(KERNEL_DIR)/library/header -I$(KERNEL_DIR) -I$(KERNEL_DIR)/library

# HOST MODE: 32-bit glibc multiarch headers (bits/wordsize.h etc.)
# are absent on a system without gcc-multilib — provide them via a
# local extraction (libc6-dev:i386) and add them to the include path.
ifeq ($(HOSTCC),1)
  INC32_DIR ?= $(HOME)/tools/inc32/usr/include/i386-linux-gnu
  INCLUDES += -I$(INC32_DIR)
endif

# LVGL include paths (added only when LVGL exists)
ifneq ($(wildcard $(LVGL_DIR)/src/lvgl.h),)
  INCLUDES += -I$(LVGL_DIR)/src -I$(KERNEL_DIR)/library/lv_port
endif

# lwIP include paths (v10.11) — for the lwIP files (gcc) AND the
# kernel/net/*.c glue (g++): lwIP headers carry __cplusplus guards.
ifneq ($(wildcard $(LWIP_DIR)/include/lwip/init.h),)
  INCLUDES += -I$(LWIP_DIR)/include -I$(KERNEL_DIR)/net -I$(KERNEL_DIR)/net/arch
endif

LDFLAGS = -m32 -ffreestanding -nostdlib -T $(LINKER_SCRIPT) $(LIBGCC_LDFLAGS)

# Files that need -mgeneral-regs-only (interrupt handlers)
# v10.7: usermode.o (called from exception-handler context) + paging.o
# v10.8: libc.o — atof() builds an IEEE-754 double via bit manipulation
#        (returned via EAX:EDX soft-float), and must not emit x87 code.
FPU_SENSITIVE = idt.o timer.o ps2_mouse.o panic.o syscall.o usermode.o paging.o libc.o net_lwip.o ne2000.o httpd.o

# ============================================================
#  Targets
# ============================================================

all: $(DIST_DIR)/kernel.elf $(DIST_DIR)/equinox.iso

# Create directories
$(BUILD_DIR)/boot $(BUILD_DIR)/kernel $(DIST_DIR):
	mkdir -p $@

# Compile assembly
$(ASM_OBJ): $(BOOT_DIR)/start.asm | $(BUILD_DIR)/boot
	$(AS) -f elf32 $< -o $@

# Compile C++ files (with correct directory creation)
$(BUILD_DIR)/kernel/%.o: $(KERNEL_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_FLAGS) $(INCLUDES) $(LVGL_FLAG) $(if $(filter $(notdir $@),$(FPU_SENSITIVE)),-mgeneral-regs-only,) -c $< -o $@

# Compile C files (kernel + lv_port)
$(BUILD_DIR)/kernel/%.o: $(KERNEL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_FLAGS) $(INCLUDES) $(LVGL_FLAG) $(if $(filter $(notdir $@),$(FPU_SENSITIVE)),-mgeneral-regs-only,) -c $< -o $@

# Compile LVGL C source
$(BUILD_DIR)/lvgl/%.o: $(LVGL_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC_C) $(COMMON_FLAGS_C) $(INCLUDES) -Wno-unused-function -Wno-unused-but-set-variable -c $< -o $@

# Compile lwIP C source (v10.11) — plain gcc + kernel headers
# (the kernel stdio.h/string.h are C-safe, proven by the LVGL build).
# -mgeneral-regs-only: this code may run from IRQ context.
$(BUILD_DIR)/lwip/%.o: $(LWIP_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC_C) $(COMMON_FLAGS_C) $(INCLUDES) -mgeneral-regs-only -w -c $< -o $@

# Link
$(DIST_DIR)/kernel.elf: $(OBJS) $(LINKER_SCRIPT) | $(DIST_DIR)
	$(LD) $(LDFLAGS) $(OBJS) -o $@ -lgcc

# ------------------------------------------------------------
#  ISO Builder (auto-rebuilds when .mrp files change)
# ------------------------------------------------------------

# Gather every .mrp + .c (mtcc samples, from test/) + .wad (large
# game data, v10.9) in dist/.
# v10.6: the dist/ layout MIRRORS the in-OS RAMFS:
#   dist/equinox/tools/*.mrp  -> RAMFS /equinox/tools (mtcc, morph_demo)
#   dist/equinox/games/*.mrp  -> RAMFS /equinox/games (snake, breakout, pong, doom)
#   dist/*.mrp              -> RAMFS root          (flat, compatibility)
#   dist/*.c                -> RAMFS /test         (mtcc samples)
# v10.9: dist/*.wad         -> RAMFS root, ZERO-COPY from module staging
#   (start.asm MODULE_STAGE 0x2800000 12 MB + fs_reference_binary) —
#   doom1.wad 4.2 MB becomes a GRUB module without touching the 2 MB kernel heap.
MRP_FILES     = $(wildcard $(DIST_DIR)/*.mrp)
MRP_TOOLS     = $(wildcard $(DIST_DIR)/equinox/tools/*.mrp)
MRP_GAMES     = $(wildcard $(DIST_DIR)/equinox/games/*.mrp)
C_FILES       = $(wildcard $(DIST_DIR)/*.c)
WAD_FILES     = $(wildcard $(DIST_DIR)/*.wad)
ALL_MODULES   = $(MRP_FILES) $(MRP_TOOLS) $(MRP_GAMES) $(C_FILES) $(WAD_FILES)

# Build ISO
$(DIST_DIR)/equinox.iso: $(DIST_DIR)/kernel.elf $(BOOT_DIR)/grub/grub.cfg $(ALL_MODULES) | $(DIST_DIR)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(DIST_DIR)/kernel.elf $(ISO_DIR)/boot/
	# Generate grub.cfg from the template + one module line per file.
	# (.c files become GRUB modules -> mrp_bootloader puts them into RAMFS
	#  /test — .mrp files land according to the dist/ structure: root or
	#  /equinox/tools, /equinox/games. mtcc sample paths are ready out of
	#  the box: mtcc /test/hello.c)
	# grub.cfg: `module` lines MUST live INSIDE the menuentry (after
	# `multiboot`) — outside an entry GRUB loads no modules at all
	# (silent: the boot log says "no boot modules"). gen_grubcfg.py replaces
	# the #MODULES_HERE marker in the template with one module line per file.
	python3 $(ROOT_DIR)/scripts/gen_grubcfg.py $(BOOT_DIR)/grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg $(DIST_DIR) $(ALL_MODULES)
	# Copy every module onto the ISO, KEEPING the dist/ folder structure
	# (so the module path in grub.cfg = the file path on the ISO).
	-cp $(MRP_FILES) $(C_FILES) $(WAD_FILES) $(ISO_DIR)/boot/ 2>/dev/null || true
	@if [ -n "$(MRP_TOOLS)" ]; then \
                mkdir -p $(ISO_DIR)/boot/equinox/tools; \
                cp $(MRP_TOOLS) $(ISO_DIR)/boot/equinox/tools/; \
	fi
	@if [ -n "$(MRP_GAMES)" ]; then \
                mkdir -p $(ISO_DIR)/boot/equinox/games; \
                cp $(MRP_GAMES) $(ISO_DIR)/boot/equinox/games/; \
	fi
	$(GRUB) -o $@ $(ISO_DIR)
	rm -rf $(ISO_DIR)

# Special target: rebuild only the ISO (no kernel rebuild)
iso: $(DIST_DIR)/equinox.iso

# v10.9: DOOM — doomgeneric multi-file build via scripts/doom_pack.py.
# Result: dist/equinox/games/doom.mrp. The WAD (doom1.wad shareware) is
# placed into dist/ manually (the makefile does NOT auto-download —
# licensing). See the DOOM section of RELEASE_v10.9.md for the full
# flow.
doom: $(DIST_DIR)
	@mkdir -p $(DIST_DIR)/equinox/games
	python3 $(ROOT_DIR)/scripts/doom_pack.py
	@echo "[doom] -> $(DIST_DIR)/equinox/games/doom.mrp"

# Raw binary (optional)
$(DIST_DIR)/kernel.bin: $(DIST_DIR)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# Run with QEMU
# v10.11: user-mode networking (slirp) + ISA NE2000.
#   guest: 10.0.2.15/24  gw 10.0.2.2 (slirp defaults; DHCP since v10.12)
#   v10.12: default hostfwd — the guest's :80 httpd is reachable from the
#   host browser at http://localhost:8080/
NET_ARGS ?= -netdev user,id=net0,hostfwd=tcp::8080-:80 -device ne2k_isa,netdev=net0,iobase=0x300,irq=9
run: $(DIST_DIR)/equinox.iso
	qemu-system-i386 -m 64 -cdrom $< $(NET_ARGS)

# Compile & pack .mrp programs
# (v10.6 routing: mrp_user/*.cpp = TOOLS -> dist/equinox/tools,
#  games/*.cpp = GAMES -> dist/equinox/games. The dist/ layout
#  structure mirrors the in-OS RAMFS — see mrp_bootloader.cpp.)
GAMES_DIR = $(ROOT_DIR)/games

pack: $(DIST_DIR)
	@mkdir -p $(DIST_DIR)/equinox/tools
	@for src in $(wildcard $(ROOT_DIR)/mrp_user/*.cpp); do \
                base=$$(basename $$src .cpp); \
                echo "[pack] $$src -> $(DIST_DIR)/equinox/tools/$$base.mrp"; \
                python3 $(ROOT_DIR)/mrp_user/mrp_pack.py $$src $(DIST_DIR)/equinox/tools/$$base.mrp || exit 1; \
	done
	@if [ -d $(GAMES_DIR) ]; then \
                mkdir -p $(DIST_DIR)/equinox/games; \
                for src in $(wildcard $(GAMES_DIR)/*.cpp); do \
                        base=$$(basename $$src .cpp); \
                        echo "[pack] $$src -> $(DIST_DIR)/equinox/games/$$base.mrp"; \
                        python3 $(ROOT_DIR)/mrp_user/mrp_pack.py $$src $(DIST_DIR)/equinox/games/$$base.mrp || exit 1; \
                done \
	fi
	@if [ -d $(TEST_DIR) ]; then \
                cp $(TEST_DIR)/*.c $(DIST_DIR)/ 2>/dev/null; \
                echo "[pack] test/*.c -> $(DIST_DIR) (ISO module -> RAMFS /test)"; \
	fi

# FIX(mtcc.c): the compiler's canonical source now lives in the PROJECT ROOT
# (mtcc.c — user request; previously only mrp_user/mtcc.cpp existed).
# This target packs from root/mtcc.c; the classic `make pack` still
# packs mrp_user/*.cpp — both produced an identical mtcc.mrp (the two
# files were kept in sync); output now lands in dist/equinox/tools/.
mtcc: $(DIST_DIR)
	@mkdir -p $(DIST_DIR)/equinox/tools
	python3 $(ROOT_DIR)/mrp_user/mrp_pack.py $(ROOT_DIR)/mtcc.c $(DIST_DIR)/equinox/tools/mtcc.mrp
	@echo "[mtcc] root/mtcc.c -> $(DIST_DIR)/equinox/tools/mtcc.mrp"

# Host-side mtcc test harness (interpreter x86-32, no qemu needed)
test:
	python3 $(ROOT_DIR)/scripts/tcc_host_test/run_tests.py

# Clean
clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR) $(ISO_DIR)

# Phony targets
.PHONY: all clean run pack iso test mtcc doom
