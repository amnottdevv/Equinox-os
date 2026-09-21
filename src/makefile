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
    LIBGCC32_DIR ?= $(HOME)/tools/root/usr/lib/gcc/x86_64-linux-gnu/14/32
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

# ------------------------------------------------------------
# BearSSL 0.6 (v0.1 TLS) — TLS 1.2 client for mget https://.
# Source: third_party/bearssl (MIT). Compiled with the DEFAULT
# configuration: the AES-NI / SSE2 / POWER8 files compile to
# empty units, so only portable constant-time code is linked
# (safe for -mgeneral-regs-only, no FPU/SSE in ring 0).
# The kernel seeds the engine RNG itself via
# br_ssl_engine_inject_entropy() (RDRAND / jitter mix,
# kernel/net/tls_client.c).
# anchors/equinox_anchors.c: 9 Mozilla root CAs (RSA + P-256
# only), generated by `brssl ta` (see anchors/README.txt).
# ------------------------------------------------------------
BEARSSL_DIR = $(ROOT_DIR)/third_party/bearssl
ifneq ($(wildcard $(BEARSSL_DIR)/inc/bearssl.h),)
  # sysrng.c compiles to a no-op stub on freestanding (no
  # BR_USE_URANDOM / BR_USE_WIN32_RAND) but still provides the
  # br_prng_seeder_system symbol that ssl_engine.c references.
  BEARSSL_SRC := $(shell find $(BEARSSL_DIR)/src -type f -name "*.c")
  BEARSSL_ANC   = $(BUILD_DIR)/bearssl/anchors/equinox_anchors.o
  BEARSSL_OBJ = $(patsubst $(BEARSSL_DIR)/src/%.c,$(BUILD_DIR)/bearssl/%.o,$(BEARSSL_SRC)) $(BEARSSL_ANC)
else
  BEARSSL_OBJ =
endif

# Object files (maintain directory structure under build/)
ASM_OBJ = $(BUILD_DIR)/boot/start.o
CPP_OBJ = $(patsubst $(KERNEL_DIR)/%.cpp,$(BUILD_DIR)/kernel/%.o,$(CPP_SRC))
C_OBJ   = $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/kernel/%.o,$(C_SRC)) $(LVGL_C_OBJ)
OBJS    = $(ASM_OBJ) $(CPP_OBJ) $(C_OBJ) $(LWIP_C_OBJ) $(BEARSSL_OBJ)

# Linker script
LINKER_SCRIPT = $(ROOT_DIR)/linker.ld

# Compiler flags
# v0.3 fix: -MMD -MP — header dependency tracking. Without it,
# a struct edit in a header (e.g. the Task clip fields) recompiles ONLY
# the .cpp files that were touched, leaving every other includer with
# the OLD struct layout: mixed ABIs in one binary (found the hard way:
# mrp_loader.o read Task::clip_h as Task::page_dir -> CR3=400 ->
# triple fault). The .d files make header edits rebuild every user.
COMMON_FLAGS   = -m32 -ffreestanding -fno-exceptions -fno-rtti -Wall -Wextra -O2 -g $(PIE_CFLAGS) -MMD -MP
COMMON_FLAGS_C = -m32 -ffreestanding -Wall -Wextra -O2 -g $(PIE_CFLAGS) -MMD -MP
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

# BearSSL include path — for the kernel/net/*.c TLS glue (g++).
ifneq ($(wildcard $(BEARSSL_DIR)/inc/bearssl.h),)
  INCLUDES += -I$(BEARSSL_DIR)/inc
endif

LDFLAGS = -m32 -ffreestanding -nostdlib -T $(LINKER_SCRIPT) $(LIBGCC_LDFLAGS)

# Files that need -mgeneral-regs-only (interrupt handlers)
# v10.7: usermode.o (called from exception-handler context) + paging.o
# v10.8: libc.o — atof() builds an IEEE-754 double via bit manipulation
#        (returned via EAX:EDX soft-float), and must not emit x87 code.
FPU_SENSITIVE = idt.o timer.o ps2_mouse.o panic.o syscall.o usermode.o paging.o libc.o net_lwip.o ne2000.o nic.o httpd.o tls_client.o task.o

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

# Compile BearSSL C source (v0.1 TLS) — plain gcc.
# inner.h AUTO-ENABLES the SSE2 and AES-NI code paths on any
# i386/amd64 GCC build; those use XMM registers (movaps/dqu) that
# this kernel cannot execute (no FPU/SSE state in ring 0, no
# 16-byte stack alignment guarantee) — a handshake that negotiates
# ChaCha20-SSE2 dies with #GP inside br_chacha20_sse2_run (found
# the hard way: panic 0x18b3ed). Force them OFF so only the
# portable constant-time implementations are linked.
# BR_RDRAND is deliberately left enabled: the bare `rdrand`
# instruction is safe in ring 0 and runtime CPUID-guarded.
# -DBR_USE_URANDOM=0 / -DBR_USE_UNIX_TIME=0: no host OS calls —
# entropy comes from br_ssl_engine_inject_entropy() (kernel
# RDRAND/jitter mix, kernel/net/tls_client.c). $(INCLUDES)
# provides the kernel's freestanding string.h (like lwIP).
$(BUILD_DIR)/bearssl/%.o: $(BEARSSL_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC_C) $(COMMON_FLAGS_C) $(INCLUDES) -I$(BEARSSL_DIR)/src -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 -mgeneral-regs-only -w -c $< -o $@

# Generated trust anchors (brssl ta output, plain C99).
$(BUILD_DIR)/bearssl/anchors/%.o: $(BEARSSL_DIR)/anchors/%.c
	@mkdir -p $(dir $@)
	$(CC_C) $(COMMON_FLAGS_C) $(INCLUDES) -DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 -mgeneral-regs-only -w -c $< -o $@

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
ELF_FILES     = $(wildcard $(DIST_DIR)/equinox/tools/*.elf)
# v0.3: userland tool C SOURCES (self-hosting — the in-OS
# `eqbuild` command compiles them; see the `tools` target).
TOOLS_C_FILES = $(wildcard $(DIST_DIR)/equinox/tools/*.c)
ALL_MODULES   = $(MRP_FILES) $(MRP_TOOLS) $(MRP_GAMES) $(C_FILES) $(WAD_FILES) $(ELF_FILES) $(TOOLS_C_FILES)

# Build ISO (v0.3: the userland tool SOURCES are staged first)
$(DIST_DIR)/equinox.iso: $(DIST_DIR)/kernel.elf $(BOOT_DIR)/grub/grub.cfg tools elfdemo $(ALL_MODULES) | $(DIST_DIR)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(DIST_DIR)/kernel.elf $(ISO_DIR)/boot/
	# Generate grub.cfg from the template + one module line per file.
	# (PATH-BASED routing in mrp_bootloader: FLAT .c -> RAMFS /test
	#  (mtcc samples); NESTED .c (equinox/tools/*.c) -> /equinox/tools
	#  (eqbuild sources); .mrp/.elf/.wad land by the dist/
	#  structure: root, /equinox/tools, /equinox/games. mtcc sample
	#  paths are ready out of the box: mtcc /test/hello.c)
	# grub.cfg: `module` lines MUST live INSIDE the menuentry (after
	# `multiboot`) — outside an entry GRUB loads no modules at all
	# (silent: the boot log says "no boot modules"). gen_grubcfg.py replaces
	# the #MODULES_HERE marker in the template with one module line per file.
	python3 $(ROOT_DIR)/scripts/gen_grubcfg.py $(BOOT_DIR)/grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg $(DIST_DIR) $(ALL_MODULES)
	# Copy every module onto the ISO, KEEPING the dist/ folder structure
	# (so the module path in grub.cfg = the file path on the ISO).
	-cp $(MRP_FILES) $(C_FILES) $(WAD_FILES) $(ISO_DIR)/boot/ 2>/dev/null || true
	# tools dir: prebuilt .mrp (mtcc.mrp + parity demos) + the .c
	# sources + ELF demos, all under boot/equinox/tools/.
	@if [ -n "$(MRP_TOOLS)$(TOOLS_C_FILES)$(ELF_FILES)" ]; then \
                mkdir -p $(ISO_DIR)/boot/equinox/tools; \
                cp $(MRP_TOOLS) $(TOOLS_C_FILES) $(ELF_FILES) $(ISO_DIR)/boot/equinox/tools/; \
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

# ---------------------------------------------------------------
# v0.2 DISK: FAT32 test/demo hard-disk image (64 MB, MBR + one
# type-0x0C FAT32 partition pre-filled with test files + doom1.wad
# when present). Built with mtools on the host; the MBR is written
# by hand (scripts/make_fat32_img.py).
# ---------------------------------------------------------------
$(DIST_DIR)/disk.img:
	python3 $(ROOT_DIR)/scripts/make_fat32_img.py

diskimg: $(DIST_DIR)/disk.img

# Boot the ISO with the FAT32 disk attached as primary master:
# the kernel auto-mounts it at /mnt (read-write). Try:
#   ls /mnt ; cat README.TXT ; doom -iwad /mnt/doom1.wad
#   cd /mnt ; mget http://<host>:<port>/file   # download to DISK
run-disk: $(DIST_DIR)/equinox.iso $(DIST_DIR)/disk.img
	qemu-system-i386 -m 64 -boot order=d -cdrom $(DIST_DIR)/equinox.iso \
                -drive file=$(DIST_DIR)/disk.img,format=raw,if=ide,index=0,media=disk $(NET_ARGS)

# FAT32 test suite (Phase A reads + Phase B writes + persistence +
# host-side mtools oracle + no-disk regression; 27 checks)
test-fat32: $(DIST_DIR)/equinox.iso
	python3 $(ROOT_DIR)/scripts/fat32_test.py

# DOOM running straight from the FAT32 disk (title-screen proof)
test-doom-disk: $(DIST_DIR)/equinox.iso $(DIST_DIR)/disk.img
	python3 $(ROOT_DIR)/scripts/doom_disk_test.py

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
# (mtcc.c; previously only mrp_user/mtcc.cpp existed).
# This target packs from root/mtcc.c; the classic `make pack` still
# packs mrp_user/*.cpp — both produced an identical mtcc.mrp (the two
# files were kept in sync); output now lands in dist/equinox/tools/.
mtcc: $(DIST_DIR)
	@mkdir -p $(DIST_DIR)/equinox/tools
	python3 $(ROOT_DIR)/mrp_user/mrp_pack.py $(ROOT_DIR)/mtcc.c $(DIST_DIR)/equinox/tools/mtcc.mrp
	@echo "[mtcc] root/mtcc.c -> $(DIST_DIR)/equinox/tools/mtcc.mrp"

# ---------------------------------------------------------------
# v0.3 (self-hosting): the userland file tools
# (ls/cat/cp/rm/mkdir/rmdir/touch/stat/mv + fstest + pipedemo) ship
# as C SOURCES in dist/equinox/tools -> RAMFS /equinox/tools. They
# are compiled INSIDE the OS by the `eqbuild` shell command (the
# in-OS mtcc, one log line per file, the .c source removed after a
# successful build) — the OS builds its own userland. The shell
# prefers the eqbuild products over its kernel builtins
# (shell_try_user_tool).
# ---------------------------------------------------------------
# (elfdemo.c is EXCLUDED: it is a host-built static ELF32 program,
#  not an mtcc .mrp — see the `elfdemo` target below.)
TOOLS_SRC  = $(filter-out %/elfdemo.c,$(wildcard $(ROOT_DIR)/tools_user/*.c))
TOOLS_HOST = $(ROOT_DIR)/scripts/tcc_host_test/mtcc_host

# Rebuild the host mtcc binary from the canonical mtcc.c source.
# (Still used by `make test` — the host test harness — but NOT by
#  the ISO build anymore: tools ship as source now.)
tools-host:
	g++ -std=gnu++17 -O2 -w -I $(ROOT_DIR)/mrp_user -o $(TOOLS_HOST) \
          $(ROOT_DIR)/scripts/tcc_host_test/host_main.cpp \
          $(ROOT_DIR)/scripts/tcc_host_test/interp32.cpp

# v0.3: ship SOURCES, not binaries. The ISO carries the .c
# files; mrp_bootloader puts them into RAMFS /equinox/tools; the
# in-OS `eqbuild` command builds + installs them. Stale prebuilt
# .mrp files of the same tools are removed so nothing prebuilt
# sneaks onto the self-hosting ISO.
tools: $(DIST_DIR)
	@mkdir -p $(DIST_DIR)/equinox/tools
	@for src in $(TOOLS_SRC); do \
                base=$$(basename $$src .c); \
                echo "[tools] $$base.c -> dist/equinox/tools/$$base.c (eqbuild source)"; \
                cp $$src $(DIST_DIR)/equinox/tools/$$base.c; \
                rm -f $(DIST_DIR)/equinox/tools/$$base.mrp; \
	done
	@echo "[tools] sources staged — run `eqbuild` in-OS to build them"

# Remove stale PREBUILT tool .mrp from dist/. The ISO prerequisites
# wildcard-snapshot dist/ at make STARTUP: if tool .mrp files exist
# when make begins and a recipe deletes them mid-run, the ISO target
# dies with "No rule to make target". Run `make clean-tools` ONCE
# after switching from the prebuild block below back to sources.
clean-tools:
	@for src in $(TOOLS_SRC); do \
                base=$$(basename $$src .c); \
                rm -f $(DIST_DIR)/equinox/tools/$$base.mrp; \
	done
	@echo "[clean-tools] stale prebuilt tool .mrp removed"

# HOST-SIDE PREBUILD (v0.3 design — DISABLED by the self-hosting flow). Uncomment this block to ship prebuilt .mrp
# tools again, e.g. for a host/mtcc parity regression ISO:
#tools: tools-host $(DIST_DIR)
#       @mkdir -p $(DIST_DIR)/equinox/tools $(BUILD_DIR)/tools
#       @for src in $(TOOLS_SRC); do \
#               base=$$(basename $$src .c); \
#               echo "[tools] $$base.c -> dist/equinox/tools/$$base.mrp"; \
#               (cd $(BUILD_DIR)/tools && $(TOOLS_HOST) c $$src) > /dev/null 2>&1; \
#               test -f $(BUILD_DIR)/tools/$$base.mrp || { echo "[tools] FAILED: $$base"; exit 1; }; \
#               cp $(BUILD_DIR)/tools/$$base.mrp $(DIST_DIR)/equinox/tools/$$base.mrp; \
#       done
#       @echo "[tools] -> $(DIST_DIR)/equinox/tools/"

# ---------------------------------------------------------------
# v0.3 (FR-07): STATIC ELF32 USER PROGRAM. Built by the HOST
# toolchain (gcc -m32 -nostdlib + mrp_user/elf_link.ld, base
# 0x01000000) — the same compiler family that builds the kernel,
# exercising the kernel ELF loader end-to-end. Ships in
# dist/equinox/tools -> RAMFS /equinox/tools (run elfdemo.elf).
# ---------------------------------------------------------------
elfdemo: $(DIST_DIR)
	@mkdir -p $(DIST_DIR)/equinox/tools
	gcc -m32 -ffreestanding -fno-pie -static -no-pie -nostdlib -O2 -Wall \
                -T $(ROOT_DIR)/mrp_user/elf_link.ld \
                $(ROOT_DIR)/tools_user/elfdemo.c \
                -o $(DIST_DIR)/equinox/tools/elfdemo.elf
	@echo "[elf] tools_user/elfdemo.c -> dist/equinox/tools/elfdemo.elf"

# Host-side mtcc test harness (interpreter x86-32, no qemu needed)
test:
	python3 $(ROOT_DIR)/scripts/tcc_host_test/run_tests.py

# Clean
clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR) $(ISO_DIR)

# Phony targets
.PHONY: all clean run run-disk diskimg pack iso test test-fat32 test-doom-disk mtcc doom tools tools-host elfdemo

# v0.3: auto-generated header dependencies (-MMD -MP)
-include $(OBJS:.o=.d)
