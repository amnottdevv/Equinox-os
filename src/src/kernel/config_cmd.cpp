/* ============================================================
 *  config_cmd.cpp — builtin shell `config` + `call` + handler bawaan
 * ------------------------------------------------------------
 *  Lihat header/library/header/config_cmd.h untuk kontraknya.
 *
 *  Modul ini juga MENDEFTKAN handler bawaan ke registry ecf_caller:
 *    set.key        <key> <val> [-> <ecf>]   tulis key=val ke .ecf
 *    set.path_local <path>                   eggkg.local = <path>
 *    set.active     <file>                   pivot store aktif
 *  (Registry terbuka — modul lain boleh menambah handler sendiri.)
 * ============================================================ */
#include "library/header/config_cmd.h"
#include "library/header/ecf.h"
#include "library/header/ecf_caller.h"
#include "library/header/fs_ram.h"
#include "library/header/stdio.h"

/* ----------------------------------------------------------------
 *  Utilitas kecil
 * ---------------------------------------------------------------- */
static int cc_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return (*a == 0 && *b == 0);
}
static void cc_copy(char* dst, int cap, const char* src) {
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
/* ambil token berikutnya (dipisah spasi) ke out; return sisa setelahnya */
static const char* cc_tok(const char* p, char* out, int cap) {
    while (*p == ' ') p++;
    int i = 0;
    while (*p && *p != ' ' && i < cap - 1) { out[i++] = *p++; }
    out[i] = '\0';
    while (*p == ' ') p++;
    return p;
}

/* ----------------------------------------------------------------
 *  Handler bawaan -> registry ecf_caller
 * ---------------------------------------------------------------- */

/* set.key <key> <val> [-> <ecf>] — tulis key=val. Default tujuan =
 * store aktif (system.ecf); `-> <ecf>` menunjuk path tertentu. */
static int h_set_key(const struct ecf_call_ctx* ctx) {
    if (ctx->argc < 2) {
        printf("set.key: need <key> <val> [-> <ecf>]\n");
        return 1;
    }
    const char* key = ctx->argv[0];
    const char* val = ctx->argv[1];
    const char* dst = 0;
    /* cari "->" sebagai pemisah tujuan */
    for (int i = 2; i < ctx->argc; i++) {
        if (cc_streq(ctx->argv[i], "->") && i + 1 < ctx->argc) {
            dst = ctx->argv[i + 1];
            break;
        }
    }
    if (!dst) {
        dst = ecf_active_path(1);
        if (!dst) { printf("set.key: no active store\n"); return 1; }
    }
    if (ecf_set_file(dst, key, val) == 0) {
        /* koherensi cache: bila tujuan = store aktif, sinkronkan store
         * in-memory supaya `set KEY` (baca) tak membaca cache lama. */
        const char* ap = ecf_active_path(0);
        if (ap && dst && cc_streq(ap, dst))
            ecf_put(ecf_store(), key, val);
        printf("[OK] set %s = %s -> %s\n", key, val, dst);
        return 0;
    }
    printf("[ERROR] set.key: gagal tulis %s\n", dst);
    return 1;
}

/* set.path_local <path> — tetapkan path lokal sebagai tujuan
 * instal (menulis eggkg.local). "Bisa dipanggil dari mana mana". */
static int h_set_path_local(const struct ecf_call_ctx* ctx) {
    if (ctx->argc < 1) {
        printf("set.path_local: need <path>\n");
        return 1;
    }
    const char* dst = ecf_active_path(1);
    if (!dst) { printf("set.path_local: no active store\n"); return 1; }
    if (ecf_set_file(dst, "eggkg.local", ctx->argv[0]) == 0) {
        /* sinkron cache store aktif (koherensi baca `set eggkg.local`) */
        const char* ap = ecf_active_path(0);
        if (ap && dst && cc_streq(ap, dst))
            ecf_put(ecf_store(), "eggkg.local", ctx->argv[0]);
        printf("[OK] eggkg.local = %s -> %s\n", ctx->argv[0], dst);
        return 0;
    }
    printf("[ERROR] set.path_local: gagal tulis %s\n", dst);
    return 1;
}

/* set.active <file> — pivot store aktif (ecf_target_set). */
static int h_set_active(const struct ecf_call_ctx* ctx) {
    if (ctx->argc < 1) {
        printf("set.active: need <file.ecf>\n");
        return 1;
    }
    if (ecf_target_set(ctx->argv[0]) == 0) {
        printf("[OK] active store -> %s\n", ctx->argv[0]);
        return 0;
    }
    printf("[ERROR] set.active: path invalid/tak bisa dibuat\n");
    return 1;
}

static int g_cfg_inited;

void config_init(void) {
    if (g_cfg_inited) return;
    g_cfg_inited = 1;
    ecf_call_register("set.key",        h_set_key);
    ecf_call_register("set.path_local", h_set_path_local);
    ecf_call_register("set.active",     h_set_active);
}

/* ----------------------------------------------------------------
 *  Perintah `config`
 * ---------------------------------------------------------------- */
static void config_list(void) {
    const char* ap = ecf_active_path(0);
    printf("store aktif : %s\n", ap ? ap : "(none)");
    const char* tp = ecf_target();
    if (tp) printf("target override (set -d): %s\n", tp);
    printf("tool configs (.config/):\n");
    static const char* const TOOLS[] = {
        "eggkg", "mtcc", "local", "system"
    };
    int found = 0;
    for (int i = 0; i < 4; i++) {
        const char* p = ecf_tool_path(TOOLS[i], 0);
        if (p) { printf("  %-8s %s\n", TOOLS[i], p); found++; }
    }
    if (!found) printf("  (belum ada — coba: eggkg init)\n");
}

static void config_show(const char* tool) {
    const char* p = ecf_tool_path(tool, 0);
    if (!p) { printf("config: %s.ecf tidak ada\n", tool); return; }
    struct fs_node* n = fs_get_node_from_path(fs_get_root(), p);
    if (!n || n->is_dir) { printf("config: %s tidak terbaca\n", p); return; }
    if (fs_ensure_content(n) != 0 || !n->content) {
        printf("config: gagal baca %s\n", p); return;
    }
    printf("--- %s ---\n", p);
    for (uint32_t i = 0; i < n->size; i++)
        printf("%c", n->content[i]);
    printf("--- end ---\n");
}

static void config_get(const char* tool, const char* key) {
    const char* p = ecf_tool_path(tool, 0);
    if (!p) { printf("config: %s.ecf tidak ada\n", tool); return; }
    const char* v = ecf_file_get(p, key);
    if (v) printf("%s.%s = %s\n", tool, key, v);
    else   printf("config: key '%s' tidak ada di %s.ecf\n", key, tool);
}

static void config_callers(void) {
    int n = ecf_call_count();
    printf("registered actions (%d):\n", n);
    for (int i = 0; i < n; i++) {
        const char* nm = ecf_call_name_at(i);
        if (nm) printf("  %s\n", nm);
    }
}

/* ----------------------------------------------------------------
 *  config init <tool> — seed template .config/<tool>.ecf
 *  (eksklusif: tak menimpa bila sudah ada; `eggkg init` = penulis
 *  resep milik eggkg sendiri, di sini hanya mtcc.)
 * ---------------------------------------------------------------- */
static const char* const CC_MTCC_TPL =
    "# mtcc.ecf — default mtcc (CLI selalu menang)\n"
    "[format]\n"
    "default = mrp\n"
    "[flags]\n"
    "default =\n"
    "[set]\n"
    "store =\n"
    "[spawn]\n"
    "name = mtcc.mrp\n"
    "args =\n";

static void config_init_tool(const char* tool) {
    if (cc_streq(tool, "mtcc")) {
        const char* p = ecf_tool_path("mtcc", 1);   /* 1 = boleh buat dir */
        if (!p) { printf("config init: tak bisa membuat .config/mtcc.ecf\n"); return; }
        struct fs_node* n = fs_get_node_from_path(fs_get_root(), p);
        if (n && !n->is_dir) {
            printf("config init: %s sudah ada (tak ditimpa)\n", p);
            return;
        }
        /* pisah parent / nama berkas (ecf_tool_path sudah membuat
         * .config, jadi parent pasti ada) */
        char dir[ECF_PATH_MAX];
        int last = -1;
        for (int i = 0; p[i]; i++) if (p[i] == '/') last = i;
        if (last <= 0) { printf("config init: path invalid\n"); return; }
        int o = 0;
        for (int i = 0; i < last && o < ECF_PATH_MAX - 1; i++) dir[o++] = p[i];
        dir[o] = '\0';
        const char* fname = p + last + 1;
        struct fs_node* parent = fs_get_node_from_path(fs_get_root(), dir);
        if (!parent || !parent->is_dir) {
            printf("config init: parent %s tidak ada\n", dir);
            return;
        }
        uint32_t len = 0;
        for (const char* s = CC_MTCC_TPL; *s; s++) len++;
        if (fs_write_binary(parent, fname, (const uint8_t*)CC_MTCC_TPL,
                            len) != 0) {
            printf("config init: gagal tulis %s\n", p);
            return;
        }
        printf("[OK] seed %s\n", p);
        return;
    }
    if (cc_streq(tool, "eggkg")) {
        printf("config init: eggkg memakai `eggkg init` (resep miliknya)\n");
        return;
    }
    printf("config init: tool '%s' tak dikenal (mtcc|eggkg)\n", tool);
}

void config_cmd(struct fs_node* cwd, const char* args) {
    (void)cwd;
    char sub[32];
    char a1[64], a2[64];
    const char* p = cc_tok(args, sub, sizeof(sub));

    if (!sub[0]) { config_list(); return; }

    if (cc_streq(sub, "list"))        { config_list(); return; }
    if (cc_streq(sub, "callers"))     { config_callers(); return; }
    if (cc_streq(sub, "show")) {
        p = cc_tok(p, a1, sizeof(a1));
        if (!a1[0]) { printf("config: butuh nama tool\n"); return; }
        config_show(a1); return;
    }
    if (cc_streq(sub, "get")) {
        p = cc_tok(p, a1, sizeof(a1));
        p = cc_tok(p, a2, sizeof(a2));
        if (!a1[0] || !a2[0]) { printf("config: butuh <tool> <key>\n"); return; }
        config_get(a1, a2); return;
    }
    if (cc_streq(sub, "init")) {
        p = cc_tok(p, a1, sizeof(a1));
        if (!a1[0]) { printf("config: butuh nama tool (init <tool>)\n"); return; }
        config_init_tool(a1); return;
    }
    printf("config: unknown '%s' (list|show|get|init|callers)\n", sub);
}

/* ----------------------------------------------------------------
 *  Perintah `call`
 * ---------------------------------------------------------------- */
void config_call_cmd(struct fs_node* cwd, const char* args) {
    (void)cwd;
    char name[ECF_CALL_NAME_MAX];
    const char* p = cc_tok(args, name, sizeof(name));
    if (!name[0]) {
        printf("call: butuh nama aksi (call <nama> [args...])\n");
        return;
    }
    struct ecf_call_ctx ctx;
    ctx.caller = "shell";
    ctx.argc = 0;
    for (int i = 0; i < ECF_CALL_ARGC_MAX; i++) ctx.argv[i] = 0;
    /* argumen: token sampai habis (batas ECF_CALL_ARGC_MAX) */
    while (ctx.argc < ECF_CALL_ARGC_MAX) {
        char t[96];
        const char* np = cc_tok(p, t, sizeof(t));
        if (!t[0]) break;
        /* simpan pointer ke buffer statis kecil — argumen dipakai
         * selama satu dispatch (sinkron), jadi aman. */
        static char argbuf[ECF_CALL_ARGC_MAX][96];
        cc_copy(argbuf[ctx.argc], 96, t);
        ctx.argv[ctx.argc] = argbuf[ctx.argc];
        ctx.argc++;
        p = np;
    }
    int rc = ecf_call(name, &ctx);
    if (rc < 0) return;   /* unknown action sudah dicetak ecf_call */
    if (rc != 0) printf("call: %s -> rc %d\n", name, rc);
}
