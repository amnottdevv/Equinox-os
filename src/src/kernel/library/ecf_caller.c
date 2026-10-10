/* ============================================================
 *  ecf_caller.c — registry aksi bernama (dispatch layer)
 * ------------------------------------------------------------
 *  Lihat header/library/header/ecf_caller.h untuk kontraknya.
 *
 *  Registry = array statis (pola ecf.c, tanpa malloc). Lookup
 *  linear — namanya ≤ 47 char, entri ≤ 48, jadi linear sudah cukup
 *  dan deterministik. Handler didaftarkan oleh modul saat init.
 * ============================================================ */
#include "library/header/ecf_caller.h"
#include "library/header/stdio.h"
#include "library/header/libstring.h"

/* ----------------------------------------------------------------
 *  Buffer statis (kernel stack kecil — semua di file-scope)
 * ---------------------------------------------------------------- */
static struct ecf_call_ent g_call[ECF_CALL_MAX];
static int                 g_ncall;

/* ----------------------------------------------------------------
 *  Utilitas kecil (libstring punya strlen/strcmp; disalin manual
 *  karena sebagian target host/inline tidak memakai libstring penuh)
 * ---------------------------------------------------------------- */
static int ec_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int ec_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return (*a == 0 && *b == 0);
}

static void ec_copy(char* dst, int cap, const char* src) {
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int ec_isnamec(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

/* ----------------------------------------------------------------
 *  Validasi bentuk nama: "<ns>.<action>[_qual]".
 *    - panjang < ECF_CALL_NAME_MAX
 *    - minimal satu '.' (namespace), tak di awal/t akhir
 *    - tanpa spasi; huruf/angka/'_'/'.'/'-' saja
 *    - tanpa dua '.' berturut ("a..b" invalid)
 *  return 1 = valid.
 * ---------------------------------------------------------------- */
int ecf_call_name_valid(const char* name) {
    if (!name || !name[0]) return 0;
    int len = ec_strlen(name);
    if (len >= ECF_CALL_NAME_MAX) return 0;
    if (name[0] == '.' || name[len - 1] == '.') return 0;
    int has_dot = 0, prev_dot = 1;   /* prev_dot=1 cegah ".." di awal */
    for (int i = 0; i < len; i++) {
        char c = name[i];
        if (!ec_isnamec(c)) return 0;
        if (c == '.') {
            if (prev_dot) return 0;      /* ".." berturut -> invalid */
            prev_dot = 1;
            has_dot = 1;
        } else {
            prev_dot = 0;
        }
    }
    return has_dot;
}

/* ----------------------------------------------------------------
 *  register
 * ---------------------------------------------------------------- */
int ecf_call_register(const char* name, ecf_call_fn fn) {
    if (!fn) return -1;
    if (!ecf_call_name_valid(name)) {
        printf("ecf_call: invalid action name '%s'\n",
               name ? name : "(null)");
        return -1;
    }
    /* duplikat -> replace (last-wins, konsisten dgn ecf key) */
    for (int i = 0; i < ECF_CALL_MAX; i++) {
        if (g_call[i].used && ec_streq(g_call[i].name, name)) {
            g_call[i].fn = fn;
            return 0;
        }
    }
    /* cari slot kosong */
    for (int i = 0; i < ECF_CALL_MAX; i++) {
        if (!g_call[i].used) {
            ec_copy(g_call[i].name, ECF_CALL_NAME_MAX, name);
            g_call[i].fn   = fn;
            g_call[i].used = 1;
            if (i >= g_ncall) g_ncall = i + 1;
            return 0;
        }
    }
    printf("ecf_call: registry full (%d) — cannot register '%s'\n",
           ECF_CALL_MAX, name);
    return -1;
}

/* ----------------------------------------------------------------
 *  dispatch
 * ---------------------------------------------------------------- */
int ecf_call(const char* name, const struct ecf_call_ctx* ctx) {
    if (!name || !name[0]) return -1;
    struct ecf_call_ent* e = NULL;
    for (int i = 0; i < ECF_CALL_MAX; i++) {
        if (g_call[i].used && ec_streq(g_call[i].name, name)) {
            e = &g_call[i];
            break;
        }
    }
    if (!e) {
        printf("[ERROR] ecf_call: unknown action '%s' (try: config "
               "callers)\n", name);
        return -1;
    }
    /* konteks defensif: bila ctx NULL, panggil dgn konteks kosong */
    struct ecf_call_ctx empty;
    if (!ctx) {
        empty.argc = 0;
        empty.caller = "ecf";
        for (int i = 0; i < ECF_CALL_ARGC_MAX; i++) empty.argv[i] = 0;
        ctx = &empty;
    }
    return e->fn(ctx);
}

/* ----------------------------------------------------------------
 *  introspeksi
 * ---------------------------------------------------------------- */
int ecf_call_exists(const char* name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < ECF_CALL_MAX; i++)
        if (g_call[i].used && ec_streq(g_call[i].name, name)) return 1;
    return 0;
}

int ecf_call_count(void) {
    int n = 0;
    for (int i = 0; i < ECF_CALL_MAX; i++) if (g_call[i].used) n++;
    return n;
}

const char* ecf_call_name_at(int i) {
    if (i < 0 || i >= ECF_CALL_MAX || !g_call[i].used) return 0;
    return g_call[i].name;
}

void ecf_call_reset(void) {
    for (int i = 0; i < ECF_CALL_MAX; i++) {
        g_call[i].used = 0;
        g_call[i].fn   = 0;
        g_call[i].name[0] = '\0';
    }
    g_ncall = 0;
}
