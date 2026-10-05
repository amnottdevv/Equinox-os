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
# ---------------------------------------------------------------------------
# v0.4.4 — HARUS nonaktifkan SSE/MMX untuk seluruh build kernel.
#
# `gcc -m32` di host x86_64 default ke -march=x86-64, sehingga -msse/-msse2
# AKTIF dan GCC bebas auto-vectorize (lihat instruksi `movaps %xmm0,0x20(%esp)`
# di ShapeImpl::appendCircle — kernel panic 0x20650E, #GP error code 0).
#
# Boot code (boot/start.asm) menyetel ESP ke stack_top tanpa menjaga
# keselarasan 16 byte sesuai psABI i386, jadi setiap akses SIMD ke stack
# bisa berakhir #GP. Ini bug kelas yang SAMA dengan yang sudah ditemukan
# di BearSSL (komentar resep BearSSL di bawah: "no 16-byte stack alignment
# guarantee") — hanya dulu diperbaiki per-file, sekarang dijadikan global.
#
# x87 TETAP aktif (-mfpmath=387) karena desktop/Calculator memakai double
# (math2.h sqrt/floor/fmod). Ring 0 memang sudah fninit + CR4.OSFXSR
# (kernel_main) dan task.cpp fxsave — jadi yang bermasalah murni
# keselarasan stack, bukan ketersediaan FPU.
# ---------------------------------------------------------------------------
SSE_OFF = -mno-mmx -mno-sse -mno-sse2 -mfpmath=387

COMMON_FLAGS   = -m32 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector $(SSE_OFF) -Wall -Wextra -O2 -g $(PIE_CFLAGS) -MMD -MP
COMMON_FLAGS_C = -m32 -ffreestanding -fno-stack-protector $(SSE_OFF) -Wall -Wextra -O2 -g $(PIE_CFLAGS) -MMD -MP
INCLUDES     = -I$(KERNEL_DIR)/library/header -I$(KERNEL_DIR) -I$(KERNEL_DIR)/library
# v0.4: port ThorVG (kernel/thorvg) + desktop EquiX — restore include
# path sesi sebelumnya (hilang saat clone project gagal). Semua .cpp
# di bawah kernel/ (termasuk kernel/thorvg/src/**) sudah auto-pickup
# oleh CPP_SRC find; ini hanya path header-nya.
INCLUDES    += -I$(KERNEL_DIR)/thorvg/inc -I$(KERNEL_DIR)/thorvg/src
INCLUDES    += -I$(KERNEL_DIR)/thorvg/src/common -I$(KERNEL_DIR)/thorvg/src/renderer
INCLUDES    += -I$(KERNEL_DIR)/thorvg/src/renderer/sw_engine -I$(KERNEL_DIR)/thorvg/src/loaders/raw

# HOST MODE: 32-bit glibc multiarch headers (bits/wordsize.h etc.)
# are absent on a system without gcc-multilib — provide them via a
# local extraction (libc6-dev:i386) and add them to the include path.
# Layout as extracted (apt-get download + dpkg -x + merge): the flat
# glibc dev headers live directly under usr/include (bits/, gnu/,
# sys/) — the old usr/include/i386-linux-gnu guess never existed.
ifeq ($(HOSTCC),1)
  INC32_DIR ?= $(HOME)/tools/inc32/usr/include
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
FPU_SENSITIVE = idt.o timer.o ps2_mouse.o panic.o syscall.o usermode.o paging.o libc.o net_lwip.o ne2000.o e1000.o nic.o httpd.o tls_client.o task.o

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
# `equinoxinstall` command compiles them; see the `tools` target).
TOOLS_C_FILES = $(wildcard $(DIST_DIR)/equinox/tools/*.c)
# v0.3.1: game C SOURCES (snake/breakout/pong rewritten from the hosted
# .cpp + Libgame versions into mtcc-dialect C, compiled IN-OS by
# `equinoxinstall` together with the tools — see the `games` target).
GAMES_C_FILES = $(wildcard $(DIST_DIR)/equinox/games/*.c)
# v0.3.2: libc C SOURCES — the 12 guarded modules + morph.h master
# include, staged into RAMFS /equinox/libc. mtcc splices them for
# #include <morph.h> (compile-time libc) and `equinoxinstall`
# check-compiles every module in-OS with 2 parallel mtcc tasks.
LIBC_C_FILES = $(wildcard $(DIST_DIR)/equinox/libc/*.c)
LIBC_H_FILES = $(wildcard $(DIST_DIR)/equinox/libc/*.h)
# v0.6: .ruf build files (mtcc -make / equinoxinstall -build) -> RAMFS
# /equinox/<name>.ruf, dipakai self-hosting TANPA equinoxinstall.
RUF_FILES     = $(wildcard $(DIST_DIR)/equinox/*.ruf)
# v0.9.2: $(MRP_GAMES) $(GAMES_C_FILES) $(WAD_FILES) keluar dari modul
# ISO — games jadi paket eggkg (doom1.wad ikut keluar; data game
# menyusul lewat paket). dist/equinox/games sudah dibersihkan.
ALL_MODULES   = $(MRP_FILES) $(MRP_TOOLS) $(C_FILES) $(ELF_FILES) $(TOOLS_C_FILES) $(LIBC_C_FILES) $(LIBC_H_FILES) $(RUF_FILES)
# v0.9.2/v0.4: NO bundled bash package. The base ISO ships without
# the bash coreutils — the canonical package lives in the Eggkg-l
# repository and is installed in-OS with:
#     eggkg update && eggkg install bash
# (eggkg itself still accepts local-path repos via
#  "eggkg update /path/package.list" — they are just not shipped.)

# v0.8: GRUB boot.img + core.img untuk Qfs -install-boot (boot dari
# disk tanpa CD). core.img dibangun grub-mkimage (biosdisk + fat +
# multiboot + normal + search; TANPA partmap — volume mkfs v0.8
# whole-disk). Keduanya distage ke RAMFS /equinox/bootimg/ supaya
# installer in-OS bisa menulisnya ke disk target.
BOOTIMG_DIR   = $(DIST_DIR)/equinox/bootimg
GRUB_MKIMAGE  := $(dir $(GRUB_PC_DIR))../bin/grub-mkimage
ifeq ($(wildcard $(GRUB_MKIMAGE)),)
  GRUB_MKIMAGE := grub-mkimage
endif
BOOTIMG_MODULES = biosdisk part_msdos fat multiboot normal echo ls

$(BOOTIMG_DIR)/boot.img $(BOOTIMG_DIR)/core.img &: boot/grub/early.cfg | $(DIST_DIR)
	@mkdir -p $(BOOTIMG_DIR)
	$(GRUB_MKIMAGE) -O i386-pc -d $(GRUB_PC_DIR) \
		-o $(BOOTIMG_DIR)/core.img -p '(hd0)/boot/grub' \
		-c boot/grub/early.cfg $(BOOTIMG_MODULES)
	cp $(GRUB_PC_DIR)/boot.img $(BOOTIMG_DIR)/boot.img
	@echo "[bootimg] boot.img + core.img ($(shell stat -c%s $(BOOTIMG_DIR)/core.img 2>/dev/null || echo '?') B) -> RAMFS /equinox/bootimg/"

# Build ISO (v0.3: the userland tool SOURCES are staged first;
# v0.3.1: the game sources too — mrp_bootloader routes NESTED .c
# (equinox/games/*.c) into RAMFS /equinox/games)
$(DIST_DIR)/equinox.iso: $(DIST_DIR)/kernel.elf $(BOOT_DIR)/grub/grub.cfg $(BOOTIMG_DIR)/boot.img $(BOOTIMG_DIR)/core.img tools libc elfdemo $(ALL_MODULES) | $(DIST_DIR)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $(DIST_DIR)/kernel.elf $(ISO_DIR)/boot/
	# v0.4: the kernel ALSO ships as a GRUB module, but stripped
	# (1.5 MB instead of 7.4 MB — the 12 MB module staging area at
	# 0x2800000 holds ~12.4 MB together with the full image, i.e. it
	# would overflow). grub.cfg names it /boot/kernel-min.elf with the
	# cmdline "boot/kernel.elf": mrp_bootloader routes by cmdline path,
	# so the RAMFS receives /boot/kernel.elf — that is the file
	# equinoxinstall copies onto the install volume.
	objcopy --strip-debug $(DIST_DIR)/kernel.elf $(ISO_DIR)/boot/kernel-min.elf
	# Generate grub.cfg from the template + one module line per file.
	# (PATH-BASED routing in mrp_bootloader: FLAT .c -> RAMFS /test
	#  (mtcc samples); NESTED .c (equinox/tools/*.c, equinox/games/*.c)
	#  -> /equinox/tools, /equinox/games (equinoxinstall sources);
	#  .mrp/.elf/.wad land by the dist/ structure: root,
	#  /equinox/tools, /equinox/games. mtcc sample paths are ready
	#  out of the box: mtcc /test/hello.c)
	# grub.cfg: `module` lines MUST live INSIDE the menuentry (after
	# `multiboot`) — outside an entry GRUB loads no modules at all
	# (silent: the boot log says "no boot modules"). gen_grubcfg.py replaces
	# the #MODULES_HERE marker in the template with one module line per file.
	python3 $(ROOT_DIR)/scripts/gen_grubcfg.py $(BOOT_DIR)/grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg $(DIST_DIR) $(ALL_MODULES) $(BOOTIMG_DIR)/boot.img $(BOOTIMG_DIR)/core.img
	# Copy every module onto the ISO, KEEPING the dist/ folder structure
	# (so the module path in grub.cfg = the file path on the ISO).
	-cp $(MRP_FILES) $(C_FILES) $(ISO_DIR)/boot/ 2>/dev/null || true
	# tools dir: prebuilt .mrp (mtcc.mrp + parity demos) + the .c
	# sources + ELF demos, all under boot/equinox/tools/.
	@if [ -n "$(MRP_TOOLS)$(TOOLS_C_FILES)$(ELF_FILES)" ]; then \
		mkdir -p $(ISO_DIR)/boot/equinox/tools; \
		cp $(MRP_TOOLS) $(TOOLS_C_FILES) $(ELF_FILES) $(ISO_DIR)/boot/equinox/tools/; \
	fi
	# v0.6: .ruf build files -> boot/equinox/ (RAMFS /equinox/*.ruf)
	@if [ -n "$(RUF_FILES)" ]; then \
		mkdir -p $(ISO_DIR)/boot/equinox; \
		cp $(RUF_FILES) $(ISO_DIR)/boot/equinox/; \
	fi
	# v0.4: no bundled bash package — install it with eggkg after boot
	# ("eggkg update && eggkg install bash"). See the note above.
	# v0.8: boot.img + core.img -> boot/equinox/bootimg/ (RAMFS
	# /equinox/bootimg/) — dibaca Qfs -install-boot saat menulis
	# bootloader ke disk target.
	@if [ -f "$(BOOTIMG_DIR)/core.img" ]; then \
		mkdir -p $(ISO_DIR)/boot/equinox/bootimg; \
		cp $(BOOTIMG_DIR)/boot.img $(BOOTIMG_DIR)/core.img $(ISO_DIR)/boot/equinox/bootimg/; \
	fi
	# games dir: the .c sources (v0.3.1) + any prebuilt .mrp that is
	# still around (doom.mrp, dbgmouse.mrp — built by `make doom` /
	# `make pack`), all under boot/equinox/games/.
	@if [ -n "$(MRP_GAMES)$(GAMES_C_FILES)" ]; then \
		mkdir -p $(ISO_DIR)/boot/equinox/games; \
		cp $(MRP_GAMES) $(GAMES_C_FILES) $(ISO_DIR)/boot/equinox/games/; \
	fi
	# libc dir: the 12 module sources + morph.h — the in-OS compiler's
	# libc (RAMFS /equinox/libc, spliced by #include <morph.h>) and
	# the equinoxinstall check-compile targets (2-thread pool).
	@if [ -n "$(LIBC_C_FILES)$(LIBC_H_FILES)" ]; then \
		mkdir -p $(ISO_DIR)/boot/equinox/libc; \
		cp $(LIBC_C_FILES) $(LIBC_H_FILES) $(ISO_DIR)/boot/equinox/libc/; \
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
	qemu-system-i386 -m 256 -cdrom $< $(NET_ARGS)

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
	qemu-system-i386 -m 256 -boot order=d -cdrom $(DIST_DIR)/equinox.iso \
		-drive file=$(DIST_DIR)/disk.img,format=raw,if=ide,index=0,media=disk $(NET_ARGS)

# ---------------------------------------------------------------
# v0.2.1 INSTALL IMAGE: dist/equinox.img — FAT32 volume labelled
# EQUINOXBASE holding /boot, /user, /equinox/{tools,games,libc},
# /test and the flat root modules.  The kernel promotes that label
# to the ROOT filesystem, so the whole userland lives ON THE DISK
# (write-through + persistent) and equinoxinstall builds into it.
# ---------------------------------------------------------------
$(DIST_DIR)/equinox.img: $(DIST_DIR)/kernel.elf $(ALL_MODULES)
	python3 $(ROOT_DIR)/scripts/make_equinox_img.py

img: $(DIST_DIR)/equinox.img

# Boot the ISO with the install image attached (SeaBIOS boots the CD,
# the kernel mounts EQUINOXBASE as /).  Try:
#   ls /equinox/tools ; cat /README.TXT ; equinoxinstall
run-img: $(DIST_DIR)/equinox.iso $(DIST_DIR)/equinox.img
	qemu-system-i386 -m 256 -boot order=d -cdrom $(DIST_DIR)/equinox.iso \
		-drive file=$(DIST_DIR)/equinox.img,format=raw,if=ide,index=0,media=disk $(NET_ARGS)

# ---------------------------------------------------------------
# v0.4 e1000: same install image, but the NIC is an Intel PRO/1000
# PCI card (8086:100E, BAR0 MMIO, INTx on PCI irq_line) instead of
# the ISA NE2000. The driver is picked at boot from
# <volume>/boot/system.ecf (.ecf, key net.driver; equinoxinstall
# step 3); with no config the
# registry falls back to registration order (ne2000, then e1000).
# ---------------------------------------------------------------
NET_ARGS_E1000 ?= -netdev user,id=net0 -device e1000,netdev=net0
run-e1000: $(DIST_DIR)/equinox.iso $(DIST_DIR)/equinox.img
	qemu-system-i386 -m 256 -boot order=d -cdrom $(DIST_DIR)/equinox.iso \
		-drive file=$(DIST_DIR)/equinox.img,format=raw,if=ide,index=0,media=disk $(NET_ARGS_E1000)

# e1000 bring-up suite: ISO + `-device e1000` (no ne2000 in the
# machine) -> lspci 8086:100E, DHCP, ping, mget, tcpping over the
# PCI card (see scripts/e1000_test.py).  The boot/system.ecf choice is
# covered separately by scripts/installer_wizard_test.py.
test-e1000: $(DIST_DIR)/equinox.iso
	python3 $(ROOT_DIR)/scripts/e1000_test.py

# FAT32 install-base suite: promote EQUINOXBASE to /, equinoxinstall
# builds onto the disk, files survive a reboot (15 checks)
test-img: $(DIST_DIR)/equinox.iso $(DIST_DIR)/equinox.img
	python3 $(ROOT_DIR)/scripts/base_img_test.py

# equinoxinstall on a PLAIN FAT32 disk (mounted at /mnt, userland in
# RAMFS): artifacts exported to /mnt/equinox/... (7 checks). The test
# builds its OWN EQDISK image — dist/disk.img is a shared artifact that
# may have been relabelled EQUINOXBASE (which would promote it to /).
test-mnt: $(DIST_DIR)/equinox.iso
	python3 $(ROOT_DIR)/scripts/mnt_build_test.py

# v0.4 interactive wizard: pick a BLANK disk, format it (fat32_mkfs),
# export the layout, choose the NIC, build onto the volume, then
# reboot and check the volume was promoted to / and boot/system.ecf
# steered the driver (15 checks)
test-wizard: $(DIST_DIR)/equinox.iso
	python3 $(ROOT_DIR)/scripts/installer_wizard_test.py

# v0.4.1 .ecf suite: builtin `set` (list/show/put/-a/-w), the eqgu
# editor + save-time mtcc compile-check, and a reboot that proves
# /boot/system.ecf on a disk steers net_init (25 checks)
test-ecf: $(DIST_DIR)/equinox.iso
	python3 $(ROOT_DIR)/scripts/ecf_test.py

# ---------------------------------------------------------------
# v0.5 instalasi MANUAL (tanpa wizard): Qfs (-list-disk / -format),
# `mount hdX` + `umount hdX`, `copy` (glob, dir auto-create, guard
# rekursif), `del` rekursif, `set -d` / `set -b` / `set -x` (skrip
# .es + /eqshell.log) dan `equinoxinstall -compile <dir>` (23 check).
# Test ini membangun DUA citra sendiri — dist/disk.img tak disentuh.
# ---------------------------------------------------------------
test-install: $(DIST_DIR)/equinox.iso
	python3 $(ROOT_DIR)/scripts/install_test.py

# ---------------------------------------------------------------
# v0.5 SATA: the FAT32 disk hangs off an AHCI controller
# (-device ahci = ich9-ahci, 8086:2922, class 01:06, BAR5 = ABAR)
# instead of the legacy PIIX IDE bus, so sector I/O has to go through
# the new AHCI driver + block layer (kernel/library/drivers/ahci.cpp).
# The CD stays on the PIIX controller and `-boot order=d` still boots
# it; the disk sits on ahci.0 = port 0 -> blk slot 4 -> `hd'e'`.
# AHCI_DISK defaults to dist/disk.img for manual `run-ahci`; the test
# target builds its OWN EQDISK image (dist/disk.img is a shared
# artifact that may have been relabelled EQUINOXBASE, which would
# promote it to / instead of mounting at /mnt).
# ---------------------------------------------------------------
AHCI_DISK ?= $(DIST_DIR)/disk.img
AHCI_ARGS ?= -boot order=d -device ahci,id=ahci \
             -drive file=$(AHCI_DISK),format=raw,if=none,id=hd0 \
             -device ide-hd,drive=hd0,bus=ahci.0
run-ahci: $(DIST_DIR)/equinox.iso $(AHCI_DISK)
	qemu-system-i386 -m 256 -cdrom $(DIST_DIR)/equinox.iso \
		$(AHCI_ARGS) $(NET_ARGS)

# AHCI bring-up suite: lspci sees the SATA controller, the disk is
# announced `SATA: ahci port 0` and mounts at /mnt, reads are
# byte-exact, writes + umount/mount round-trip, the FAT sector cache
# climbs, and a file survives a reboot (17 checks)
test-ahci: $(DIST_DIR)/equinox.iso
	python3 $(ROOT_DIR)/scripts/ahci_test.py

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
# are compiled INSIDE the OS by the `equinoxinstall` shell command
# (the in-OS mtcc, one log line per file, the .c source removed
# after a successful build) — the OS builds its own userland. The
# shell prefers the equinoxinstall products over its kernel
# builtins (shell_try_user_tool).
# ---------------------------------------------------------------
# (elfdemo.c is EXCLUDED: it is a host-built static ELF32 program,
#  not an mtcc .mrp — see the `elfdemo` target below.)
# v0.9.2/v0.4 — the bash-class coreutils (ls cat cp mv mkdir rmdir rm
# touch stat) are NOT part of the base ISO at all: their canonical
# sources live in the Eggkg-l package repository and the user installs
# them in-OS with `eggkg update && eggkg install bash`. The shell keeps
# builtins + aliases for all of them, and the text filters
# (grep/wc/rev/...) stay in the base so v0.8 pipes are useful
# out-of-the-box. tools_user/ therefore contains no bash-class source.
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
# in-OS `equinoxinstall` command builds + installs them. Stale
# prebuilt .mrp files of the same tools are removed so nothing
# prebuilt sneaks onto the self-hosting ISO.
tools: $(DIST_DIR)
	@mkdir -p $(DIST_DIR)/equinox/tools
	@for src in $(TOOLS_SRC); do \
		base=$$(basename $$src .c); \
		echo "[tools] $$base.c -> dist/equinox/tools/$$base.c (equinoxinstall source)"; \
		cp $$src $(DIST_DIR)/equinox/tools/$$base.c; \
		rm -f $(DIST_DIR)/equinox/tools/$$base.mrp; \
	done
	@echo "[tools] sources staged — run `equinoxinstall` in-OS to build them"

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

# ---------------------------------------------------------------
# v0.3.1 (self-hosting games): snake, breakout and pong ship as
# mtcc-dialect C SOURCES in dist/equinox/games -> RAMFS
# /equinox/games, next to the still-prebuilt doom.mrp /
# dbgmouse.mrp (host-built). `equinoxinstall` compiles them in-OS
# with mtcc exactly like the tools — the games became part of the
# self-hosting userland (the old .cpp + Libgame hosted builds stay
# in games/ for reference/parity, but the ISO ships the .c).
# ---------------------------------------------------------------
GAMES_SELFHOST = snake breakout pong

games: $(DIST_DIR)
	@mkdir -p $(DIST_DIR)/equinox/games
	@for base in $(GAMES_SELFHOST); do \
		echo "[games] $$base.c -> dist/equinox/games/$$base.c (equinoxinstall source)"; \
		cp $(GAMES_DIR)/$$base.c $(DIST_DIR)/equinox/games/$$base.c; \
		rm -f $(DIST_DIR)/equinox/games/$$base.mrp; \
	done
	@echo "[games] sources staged — run `equinoxinstall` in-OS to build them"

# Remove the stale PREBUILT game .mrp from a v0.3 dist/ tree.
# Same wildcard-snapshot caveat as clean-tools: run ONCE before the
# first `make` after switching games from prebuilt to sources.
clean-games:
	@for base in $(GAMES_SELFHOST); do \
		rm -f $(DIST_DIR)/equinox/games/$$base.mrp; \
	done
	@echo "[clean-games] stale prebuilt game .mrp removed"

# ---------------------------------------------------------------
# v0.3.2 (modular libc): the Equinox OS libc ships as 12 guarded
# C modules + morph.h in dist/equinox/libc -> RAMFS /equinox/libc.
# mtcc splices morph.h for #include <morph.h>/<stdio.h>/... (the
# compile-time libc), and `equinoxinstall` check-compiles every
# module in-OS with a 2-thread parallel mtcc pool (--lib -q -c).
# The sources STAY in RAMFS (unlike tools/games) — the compiler
# needs them for every later splice.
# ---------------------------------------------------------------
libc: $(DIST_DIR)
	@mkdir -p $(DIST_DIR)/equinox/libc
	@for src in $(ROOT_DIR)/libc/*.c $(ROOT_DIR)/libc/morph.h; do \
		echo "[libc] $$src -> dist/equinox/libc/$$(basename $$src)"; \
		cp $$src $(DIST_DIR)/equinox/libc/; \
	done
	@rm -f $(DIST_DIR)/equinox/libc/*.mrp
	@echo "[libc] 12 modules + morph.h staged — spliced by mtcc, checked by equinoxinstall"

# Remove stale check-compile artifacts (mtcc --lib -c writes .mrp
# next to the module; equinoxinstall deletes them after verifying).
clean-libc:
	@rm -f $(DIST_DIR)/equinox/libc/*.mrp
	@echo "[clean-libc] stale libc .mrp removed"

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
	gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -static -no-pie -nostdlib -O2 -Wall \
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
.PHONY: all clean run run-disk diskimg pack iso test test-fat32 test-doom-disk test-ecf test-install mtcc doom tools tools-host elfdemo games libc clean-games clean-tools clean-libc

# v0.3: auto-generated header dependencies (-MMD -MP)
-include $(OBJS:.o=.d)
