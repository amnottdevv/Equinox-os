/*
 * ============================================================================
 *  mrp_bootloader.cpp — Ingest GRUB multiboot modules into RAMFS
 * ----------------------------------------------------------------------------
 *  Workflow_mrp.md Section A "Blockers that MUST be resolved first" says:
 *  "there is no path to put .mrp files into RAMFS from outside QEMU."
 *
 *  The solution recommended by that workflow: GRUB multiboot modules.
 *  grub.cfg:
 *      module /boot/hello.mrp
 *  Then kernel_main() reads mb_info->mods_addr and, for each entry, copies
 *  the byte range [mod_start, mod_end) into RAMFS via fs_write_binary().
 *
 *  This file contains:
 *    - mrp_bootloader_load_modules(mb_info): entry point, called from
 *      kernel_main() AFTER fs_init() (it needs the root fs).
 *    - Internal helper: extract the basename from the cmdline (e.g.
 *      "/boot/hello.mrp" -> "hello.mrp") so file names in RAMFS stay
 *      short & path-free.
 *
 *  Security notes:
 *    - Module data is already mmap'd by GRUB at physical addresses the
 *      kernel can read (low 4MB in the Equinox OS configuration). No
 *      re-mapping needed.
 *    - We trust GRUB — no validation that mod_start <= mod_end, because
 *      the GRUB spec guarantees it. But a defensive check is still there.
 *    - Cmdline strings are NUL-terminated (GRUB spec guarantee).
 * ============================================================================
 */

#include "header/multiboot.h"
#include "header/fs_ram.h"
#include "header/stdio.h"
#include "header/color.h"       // VGA_COLOR_* for the boot file listing
#include "header/libstring.h"   // strncmp, snprintf (bounded)
#include "header/panic.h"       // kernel_panic for libc load failure
#include "header/timer.h"       // sleep_ms (boot-list pacing)
#include <stdint.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
//  Human-readable size for the boot file listing ("4.2 MB", "18.1 KB").
//  Prints into the caller's buffer; never longer than 10 chars + NUL.
// ----------------------------------------------------------------------------
static void human_size(uint32_t bytes, char* buf, size_t bufsize) {
    if (bytes >= 1024u * 1024u) {
        snprintf(buf, bufsize, "%u.%u MB", bytes / (1024u * 1024u),
                 (bytes % (1024u * 1024u)) * 10 / (1024u * 1024u));
    } else if (bytes >= 1024u) {
        snprintf(buf, bufsize, "%u.%u KB", bytes / 1024u,
                 (bytes % 1024u) * 10 / 1024u);
    } else {
        snprintf(buf, bufsize, "%u B", bytes);
    }
}

// ----------------------------------------------------------------------------
//  v10.9 "WAD PATH" — routing BIG modules via zero-copy reference.
// ----------------------------------------------------------------------------
//  start.asm moves ALL GRUB modules to MODULE_STAGE (0x2800000+, 12 MB,
//  supervisor pages) before .bss is cleared. Small modules (~150 KB
//  total: mtcc, games, sample .c) are still COPIED into the kernel heap
//  as before (fs_write_binary). Modules >= BIGMOD_THRESHOLD (e.g. the
//  4.2 MB doom1.wad) do NOT fit in the 2 MB KERNEL_HEAP — their RAMFS
//  file is created via fs_reference_binary: the node points DIRECTLY at
//  the bytes in staging, with no copy. Reading the file (read/readfile/
//  lseek syscalls) stays transparent.
// ----------------------------------------------------------------------------
#define BIGMOD_THRESHOLD (128u * 1024u)   /* 128 KB */

// ----------------------------------------------------------------------------
//  Helper: extract the basename from a cmdline path.
//  "/boot/hello.mrp" -> "hello.mrp"
//  "hello.mrp"       -> "hello.mrp"
//  Returns a pointer inside the cmdline string (no new allocation).
// ----------------------------------------------------------------------------
static const char* basename_of(const char* path) {
    if (!path) return "unknown.mrp";
    const char* last_slash = nullptr;
    for (const char* p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    return last_slash ? last_slash + 1 : path;
}

// ----------------------------------------------------------------------------
//  System layout + path-based module routing.
//
//  The in-OS RAMFS now has a permanent structure:
//      /user            — user space (shell home directory)
//      /equinox/tools   — global tools (mtcc.mrp, morph_demo.mrp)
//      /equinox/games   — games (snake.mrp, breakout.mrp, pong.mrp)
//
//  ensure_system_layout() creates those folders (idempotently) so the
//  structure ALWAYS exists even if no module populates their contents.
// ----------------------------------------------------------------------------
static void ensure_system_layout(struct fs_node* root) {
    /* /user — user home space */
    if (!fs_find_child(root, "user")) {
        fs_create_dir(root, "user");
    }
    /* /equinox + the tools/games subfolders */
    struct fs_node* sys = fs_find_child(root, "equinox");
    if (!sys) {
        if (fs_create_dir(root, "equinox") == 0) {
            sys = fs_find_child(root, "equinox");
        }
    }
    if (sys) {
        if (!fs_find_child(sys, "tools")) fs_create_dir(sys, "tools");
        if (!fs_find_child(sys, "games")) fs_create_dir(sys, "games");
    }
}

// ----------------------------------------------------------------------------
//  ensure_dir_path() — walks "a/b/c" from the root, creates every
//  component that does not exist yet (fs_create_dir kept idempotent by a
//  find-first check), returns the LAST directory node. NULL if any
//  component turns out to be a FILE.
// ----------------------------------------------------------------------------
static struct fs_node* ensure_dir_path(struct fs_node* root, const char* path) {
    struct fs_node* cur = root;
    char part[64];
    int idx = 0;
    for (const char* p = path; ; p++) {
        if (*p == '/' || *p == '\0') {
            if (idx > 0) {
                part[idx] = '\0';
                struct fs_node* next = fs_find_child(cur, part);
                if (!next) {
                    if (fs_create_dir(cur, part) != 0) return NULL;
                    next = fs_find_child(cur, part);
                    if (!next) return NULL;
                } else if (!next->is_dir) {
                    return NULL;      /* component clashes with a file */
                }
                cur = next;
                idx = 0;
            }
            if (*p == '\0') break;
        } else {
            if (idx >= (int)sizeof(part) - 1) return NULL;   /* >63: skip */
            part[idx++] = *p;
        }
    }
    return cur;
}

// ----------------------------------------------------------------------------
//  mrp_bootloader_load_modules — entry point from kernel_main()
//  Returns the number of modules successfully loaded into the RAMFS root.
// ----------------------------------------------------------------------------
extern "C" int mrp_bootloader_load_modules(const multiboot_info_t* mb_info) {
    if (!mb_info) {
        printf("mrp_boot: no multiboot info\n");
        return 0;
    }

    // Check the MODS flag (bit 3). If not set, mods_count/mods_addr
    // are invalid. The libc probe result completes the same line.
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    printf("     probing libc module...");
    if (!(mb_info->flags & MULTIBOOT_INFO_MODS)) {
        printf(" not found (no boot modules)\n");
        return 0;
    }

    uint32_t count = mb_info->mods_count;
    if (count == 0) {
        printf(" not found (0 modules)\n");
        return 0;
    }

    // mods_addr is a physical address. Since Equinox OS has not set up paging
    // yet (identity mapping), physical == virtual at this boot phase. Direct cast.
    const struct multiboot_mod_entry* mods =
        (const struct multiboot_mod_entry*)(uintptr_t)mb_info->mods_addr;

    struct fs_node* root = fs_get_root();
    if (!root) {
        printf("mrp_boot: RAMFS root not initialized — call this after fs_init()\n");
        return 0;
    }

    /* Create the system layout first (idempotently) — /user as the
     * user home space + /equinox/tools + /equinox/games for global
     * files — so the structure ALWAYS exists even if the ISO carries
     * no modules for them. */
    ensure_system_layout(root);
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    printf("     system layout: /user, /equinox/tools, /equinox/games\n");

    /* The /test directory — .c files (samples for mtcc) are placed here
     * so the RAMFS root stays tidy (only user files + system folders).
     * Created on demand: with no .c modules, the folder is not created
     * at all. */
    struct fs_node* testdir = NULL;

    /* Detect libc among the modules (name starts with "libc", e.g.
     * "libc.mrp"). The result completes the "Detecting libc..." line.
     * libc is not mandatory — .mrp programs use the kernel's built-in
     * API. */
    int libc_module = 0;
    for (uint32_t i = 0; i < count; i++) {
        const char* nm = basename_of((const char*)(uintptr_t)mods[i].cmdline);
        if (strncmp(nm, "libc", 4) == 0) { libc_module = 1; break; }
    }
    printf("%s", libc_module ? " found\n"
                             : " not found (using kernel built-in API)\n");
    int loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        const char* cmdline = (const char*)(uintptr_t)mods[i].cmdline;
        const char* name = basename_of(cmdline);

        uint32_t start = mods[i].mod_start;
        uint32_t end   = mods[i].mod_end;

        // Defensive: skip zero-length or inverted modules
        if (end <= start) {
            printf("Skip %s (invalid module range 0x%x..0x%x)\n",
                   name, start, end);
            continue;
        }

        uint32_t len = end - start;
        const uint8_t* data = (const uint8_t*)(uintptr_t)start;

        /* Module routing (PATH-BASED, generic — not a name list):
         *   cmdline "equinox/tools/mtcc.mrp" -> RAMFS /equinox/tools/mtcc.mrp
         *     (folders created on demand by ensure_dir_path)
         *   flat cmdline "mtcc.mrp"  -> RAMFS root   (legacy compatibility)
         *   flat cmdline "hello.c"   -> RAMFS /test  (mtcc samples)
         * grub.cfg (gen_grubcfg.py) writes cmdline = the dist-relative
         * path, so the ISO /boot/equinox/... structure automatically maps
         * onto the RAMFS /equinox/... structure without hardcoding file
         * names in the kernel. */
        const char* route = cmdline;
        while (*route == '/') route++;               /* strip leading '/' */

        /* find the LAST '/' as the dir/file separator */
        const char* last_slash = nullptr;
        for (const char* q = route; *q; q++) {
            if (*q == '/') last_slash = q;
        }

        struct fs_node* dest = NULL;
        char dirpart[192];
        if (last_slash) {
            /* nested: copy the directory part, walk + create folders */
            uint32_t dlen = (uint32_t)(last_slash - route);
            if (dlen >= sizeof(dirpart)) {
                printf("Skip %s (path too deep)\n", name);
                continue;
            }
            for (uint32_t i = 0; i < dlen; i++) dirpart[i] = route[i];
            dirpart[dlen] = '\0';
            dest = ensure_dir_path(root, dirpart);
            if (!dest) {
                printf("Failed to create /%s for %s\n", dirpart, name);
                continue;
            }
        }

        size_t namelen = 0;
        while (name[namelen]) namelen++;
        int is_csrc = (namelen >= 2 && name[namelen-1] == 'c' &&
                       name[namelen-2] == '.');

        if (!dest) {
            /* flat: legacy rule — .c -> /test, everything else -> root */
            if (is_csrc) {
                if (!testdir) {
                    testdir = fs_find_child(root, "test");
                    if (!testdir) {
                        if (fs_create_dir(root, "test") == 0) {
                            testdir = fs_find_child(root, "test");
                        }
                    }
                }
                dest = testdir ? testdir : root;
                /* fall back to root if /test cannot be created */
            } else {
                dest = root;
            }
        }

        /* v10.9: big files -> zero-copy reference to module staging
         * (see the BIGMOD_THRESHOLD comment above). Small files ->
         * copy into the kernel heap (the old behavior, still cheap). */
        int ret;
        if (len >= BIGMOD_THRESHOLD) {
            ret = fs_reference_binary(dest, name, data, len);
        } else {
            ret = fs_write_binary(dest, name, data, len);
        }
        if (ret == 0) {
            /* Boot file listing — every file included on the ISO gets one
             * green [ OK ] line with its full RAMFS path and size, so the
             * user can see exactly what was loaded at boot. Small pacing
             * per line keeps the list readable (timer is alive by now). */
            char pbuf[256];
            if (dest == testdir) {
                snprintf(pbuf, sizeof(pbuf), "/test/%s", name);
            } else if (dest == root) {
                snprintf(pbuf, sizeof(pbuf), "/%s", name);
            } else {
                fs_get_path(dest, pbuf, sizeof(pbuf));
                size_t plen = 0;
                while (pbuf[plen] && plen + 1 < sizeof(pbuf)) plen++;
                pbuf[plen] = '/';
                pbuf[plen + 1] = '\0';
                size_t nl = 0;
                while (name[nl] && plen + 2 + nl < sizeof(pbuf)) {
                    pbuf[plen + 1 + nl] = name[nl];
                    nl++;
                }
                pbuf[plen + 1 + nl] = '\0';
            }
            char hsize[16];
            human_size(len, hsize, sizeof(hsize));
            set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            printf("[ OK ] ");
            set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            printf("%s", pbuf);
            set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
            printf("  (%s%s)\n", hsize,
                   (len >= BIGMOD_THRESHOLD) ? ", zero-copy staged" : "");
            set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            /* mrp_bootloader always runs after timer_init() in
             * kernel_main(), so sleep_ms() is safe here. */
            sleep_ms(80);
            loaded++;
        } else {
            /* libc failing to enter RAMFS = fatal for .mrp programs
             * that need it -> panic + 30-second auto-reboot.
             * (A real-world example of the kernel_panic "can't load
             * libc" path.) */
            if (strncmp(name, "libc", 4) == 0) {
                char detail[96];
                snprintf(detail, sizeof(detail),
                         "fs_write_binary('%s') failed, err=%d", name, ret);
                kernel_panic("can't load libc", detail);
            }
            printf("Failed to add %s (err=%d)\n", name, ret);
        }
    }

    set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    printf("[ OK ] ");
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    printf("%d file(s) included from %u boot module(s)\n", loaded, count);
    return loaded;
}
