/* ============================================================
 *  eggkg.cpp — package manager Equinox OS (0.4 Beta)
 * ------------------------------------------------------------
 *  BUILTIN shell (ring 0). Filosofi Gentoo/portage mini:
 *  paket = sumber kode + resep .ruf, dibangun di mesin tujuan
 *  oleh mtcc (spawn "mtcc -make"), hasil .mrp disalin ke /bin.
 *
 *  Sumber paket (server, urut prioritas):
 *    1. argumen eksplisit:   eggkg update <url|/path/package.list>
 *    2. ecf  [eggkg] server: system.ecf aktif
 *    3. default:             raw.githubusercontent.com/amnottdevv/
 *                            Eggkg-l/main/package.list
 *
 *  Dua format server didukung:
 *    - package.list (v0, repo Eggkg-l):
 *        bash = ["https://github.com/o/r/blob/main/bash/cat.c", ...]
 *      URL blob dikonversi otomatis ke raw.githubusercontent.com.
 *      URL ber-path absolut ("/equinox/repo/bash/cat.c") dibaca
 *      langsung dari FS — repo offline di disk/RAMFS.
 *    - index.idx (v1, INI per paket):
 *        [bash] version/size/sha256/url/mirror/deps
 *      Dipakai untuk verifikasi sha256 + mirror fallback. Bila
 *      index.idx tidak ada di server, package.list tetap sah
 *      (verifikasi hash dilewati — dilaporkan jujur).
 *
 *  Layout lokal (usulan user, dikunci):
 *    /equinox/.local/<pkg>/          workspace (src/ + build.ruf)
 *    /equinox/.local/<pkg> (file .mrp) hasil build (out)
 *    /bin/<nama>.mrp                 salinan terpasang (system path)
 *    /equinox/.local/installed.db    basis data terpasang
 *    /equinox/.local/package.list    salinan server (eggkg update)
 *    /equinox/.local/index.idx       salinan server (bila ada)
 * ============================================================ */

#include "library/header/stdio.h"
#include "library/header/libstring.h"
#include "library/header/fs_ram.h"
#include "library/header/ecf.h"
#include "library/header/eggkg.h"
#include "library/header/egg_sha256.h"
#include "library/header/malloc.h"
#include "net/net.h"
#include <stdint.h>

extern "C" uint32_t sys_now(void);

/* ============================================================
 *  Konstanta
 * ============================================================ */
#define EGG_LOCAL_DEFAULT   "/equinox/.local"
#define EGG_BIN_DIR         "/bin"
#define EGG_LIST_NAME       "package.list"
#define EGG_INDEX_NAME      "index.idx"
#define EGG_DB_NAME         "installed.db"

#define EGG_SERVER_DEFAULT \
    "https://raw.githubusercontent.com/amnottdevv/Eggkg-l/main/" \
    "package.list"

#define EGG_MAX_PKG     12
#define EGG_MAX_SRC     24
#define EGG_MAX_MIRROR  3
#define EGG_MAX_FILES   28
#define EGG_URL_MAX     190
#define EGG_FETCH_CAP   (384 * 1024)      /* mode arsip (v2/v1 .ruf)   */
#define EGG_NET_CAP     (64 * 1024)       /* package.list/sumber .c kecil */
/* Lesson 0.4 Beta: heap kernel = ~45 KB (main) + ~1 MB (ext). Buffer 384 KB
 * yang dipegang SELAMA build mtcc menyebabkan fragmentasi -> 3 job
 * "GAGAL tulis" + malloc gagal. Buf 384 KB hanya untuk mode arsip dan
 * dibebaskan SEBELUM spawn mtcc. */
#define EGG_DB_MAX      4096

/* ============================================================
 *  Model data index (gabungan package.list + index.idx)
 * ============================================================ */
struct egg_entry {
    char name[40];
    char version[20];                     /* "" = tanpa versi (v0)   */
    char desc[96];
    char sha[72];                         /* hex 64 + slop           */
    uint32_t size;                        /* 0 = tak diketahui       */
    char url[EGG_URL_MAX];                /* arsip paket (index v1)  */
    char mirror[EGG_MAX_MIRROR][EGG_URL_MAX];
    int  nmirror;
    char deps[96];
    char src[EGG_MAX_SRC][EGG_URL_MAX];   /* URL .c (package.list)   */
    int  nsrc;
};

struct egg_installed {
    char name[40];
    char version[20];
    char files[EGG_MAX_FILES][48];
    int  nfiles;
    char deps[96];
};

/* IMPORTANT (0.4 Beta memory budget): the package table is allocated
 * DYNAMICALLY (malloc on the kernel heap), not from static BSS. BSS may
 * grow to 0x600000 (headroom, the heap starts above it) — do not add
 * another ~1 MB of statics: it now consumes bss/heap headroom instead
 * of the MRP arena. */
static struct egg_entry*    g_pkg;      /* EGG_MAX_PKG entri           */
static int                  g_npkg;
static char                 g_index_rev[24];
static char                 g_repo_base[EGG_URL_MAX];  /* base dir repo */

static int egg_ensure_tables(void) {
    if (!g_pkg) {
        g_pkg = (struct egg_entry*)malloc(sizeof(struct egg_entry)
                                          * EGG_MAX_PKG);
        if (!g_pkg) return -1;
        memset(g_pkg, 0, sizeof(struct egg_entry) * EGG_MAX_PKG);
    }
    return 0;
}

/* ============================================================
 *  Animasi — egg_progress (desain §9)
 *  Spinner + bar; semua lewat \r (FIX S6: CR = kolom 0) dan
 *  pad spasi. Di-pipe/serial tetap terbaca karena tiap tahap
 *  selalu ditutup baris [ok]/[gagal] permanen.
 * ============================================================ */
static uint32_t g_last_render;

static void egg_bar(const char* label, uint32_t got, uint32_t total) {
    /* bar 20 kolom + persen; total 0 = spinner tak tentu */
    char line[110];
    int  pos = 0;
    uint32_t now = sys_now();
    if (g_last_render && (now - g_last_render) < 120) return;
    g_last_render = now;

    if (total == 0) {
        static const char fr[5] = { '|', '/', '-', '\\', 0 };
        static uint32_t frame;
        for (const char* s = label; *s && pos < 100; s++)
            line[pos++] = *s;
        line[pos++] = ' ';
        line[pos++] = fr[frame & 3];
        line[pos++] = 0;
        printf("\r%s", line);
        frame++;
        return;
    }

    uint32_t pct = (total) ? (got * 100 / total) : 0;
    if (pct > 100) pct = 100;
    int filled = (int)((got * 20) / (total ? total : 1));
    if (filled > 20) filled = 20;

    line[pos++] = '[';
    for (int i = 0; i < 20; i++)
        line[pos++] = (i < filled) ? '#' : '-';
    line[pos++] = ']';
    line[pos++] = ' ';
    /* persen manual (printf kernel tanpa %u pad) */
    {
        char tmp[8];
        int  ti = 0;
        if (pct == 0) tmp[ti++] = '0';
        while (pct > 0) { tmp[ti++] = (char)('0' + (pct % 10)); pct /= 10; }
        while (ti > 0) line[pos++] = tmp[--ti];
        line[pos++] = '%';
    }
    line[pos] = 0;
    printf("\r%s", line);
}

static void egg_line_clear(void) {
    /* tutup baris animasi: CR + 78 spasi + CR */
    printf("\r                                                                                \r");
}

static void egg_stage(int idx, int total, const char* text) {
    printf("[%d/%d] %s\n", idx, total, text);
}

static void egg_ok(const char* note)   { printf("      [ok] %s\n", note); }
static void egg_fail(const char* note) { printf("      [gagal] %s\n", note); }

/* ============================================================
 *  Util string kecil
 * ============================================================ */
static void egg_trim(char* s) {
    int n = 0;
    while (s[n]) n++;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
    int st = 0;
    while (s[st] == ' ' || s[st] == '\t') st++;
    if (st > 0) {
        int i = 0;
        while (s[st]) s[i++] = s[st++];
        s[i] = 0;
    }
}

static int egg_ieq(const char* a, const char* b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0);
}

static void egg_copy_str(const char* s, char* out, int outmax);

/* printf kernel tidak punya precision %.Ns — potong manual */
static void egg_hex16(const char* hex, char* out) {
    int i = 0;
    for (; hex[i] && i < 16; i++) out[i] = hex[i];
    out[i] = 0;
}

static int egg_icontains(const char* hay, const char* needle) {
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j]) {
            char x = hay[i + j], y = needle[j];
            if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
            if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
            if (!hay[i + j] || x != y) break;
            j++;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

/* github blob/raw -> raw.githubusercontent (blob = halaman HTML) */
static void egg_url_to_raw(const char* in, char* out, int outmax) {
    /* pola: https://github.com/OWNER/REPO/blob/REF/PATH
     *       https://github.com/OWNER/REPO/raw/REF/PATH        */
    out[0] = 0;
    if (memcmp(in, "https://github.com/", 19) == 0) {
        const char* p = in + 19;              /* OWNER/REPO/verb/...  */
        const char* slash1 = 0, *slash2 = 0, *verb = 0;
        for (const char* q = p; *q; q++) {
            if (*q == '/') {
                if (!slash1) slash1 = q;
                else if (!slash2) slash2 = q;
                else { verb = q; break; }
            }
        }
        (void)slash1;
        if (slash2 && verb && (verb - slash2 == 5) &&
            memcmp(slash2, "/blob", 5) == 0) {
            /* tulis ulang: raw.githubusercontent.com/OWNER/REPO/REF/PATH */
            int o = 0;
            const char* pre = "https://raw.githubusercontent.com/";
            for (const char* s = pre; *s && o < outmax - 1; s++)
                out[o++] = *s;
            for (const char* s = p; s < slash2 && o < outmax - 1; s++)
                out[o++] = *s;                /* OWNER/REPO           */
            if (o < outmax - 1) out[o++] = '/';
            for (const char* s = verb + 1; *s && o < outmax - 1; s++)
                out[o++] = *s;                /* REF/PATH             */
            out[o] = 0;
            return;
        }
        if (slash2 && verb && (verb - slash2 == 4) &&
            memcmp(slash2, "/raw", 4) == 0) {
            int o = 0;
            const char* pre = "https://raw.githubusercontent.com/";
            for (const char* s = pre; *s && o < outmax - 1; s++)
                out[o++] = *s;
            for (const char* s = p; s < slash2 && o < outmax - 1; s++)
                out[o++] = *s;
            if (o < outmax - 1) out[o++] = '/';
            for (const char* s = verb + 1; *s && o < outmax - 1; s++)
                out[o++] = *s;
            out[o] = 0;
            return;
        }
    }
    /* selain itu: salin apa adanya */
    int o = 0;
    for (const char* s = in; *s && o < outmax - 1; s++)
        out[o++] = *s;
    out[o] = 0;
}

/* derive base dir repo dari satu URL sumber:
 *   https://raw.githubusercontent.com/o/r/main/bash/cat.c
 *     -> https://raw.githubusercontent.com/o/r/main/
 *   /equinox/repo/bash/cat.c -> /equinox/repo/bash/            */
static void egg_repo_base_from(const char* url, char* out, int outmax) {
    out[0] = 0;
    int n = 0;
    while (url[n]) n++;
    while (n > 0 && url[n - 1] != '/') n--;
    if (n == 0) return;
    if (n >= outmax) n = outmax - 1;
    for (int i = 0; i < n; i++) out[i] = url[i];
    out[n] = 0;
}

/* ============================================================
 *  Parser package.list (v0)
 *  Format:  nama = ["url1", "url2", ...]
 *  Toleran: tanpa kutip, satu URL, koma ganda, komentar '#'
 * ============================================================ */
/* buffer parser — STATIS (kernel stack kecil; parser non-reentrant,
 * dipanggil berurutan dari konteks shell). 8 KB menampung 24 URL
 * (~190 char masing-masing) dalam SATU baris fisik sekalipun.
 * CATATAN: pakai konstanta eksplisit, BUKAN sizeof(ptr). */
#define EGG_ACC_SIZE 8192
#define EGG_LINE_SIZE 8192
static char egg_pl_acc[EGG_ACC_SIZE];
static char egg_pl_line[EGG_LINE_SIZE];

static void egg_parse_list(const char* text) {
    if (egg_ensure_tables() != 0) return;
    g_npkg = 0;
    g_index_rev[0] = 0;

    const char* p = text;
    while (*p && g_npkg < EGG_MAX_PKG) {
        /* Kumpulkan SATU entri logis — boleh multi-baris (format
         * repo bisa "nama = [\n url1,\n url2\n]") atau SATU baris
         * panjang (19 URL ~1.5 KB). */
        char* acc = egg_pl_acc;
        int  al = 0;
        int  have_eq = 0, have_open = 0, done = 0;

        while (*p && !done) {
            char* line = egg_pl_line;
            int  ll = 0;
            while (*p && *p != '\n' && ll < EGG_LINE_SIZE - 2)
                line[ll++] = *p++;
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            line[ll] = 0;

            char* t = line;
            while (*t == ' ' || *t == '\t') t++;

            /* junk sebelum entri: kosong / komentar */
            if (al == 0 && (*t == 0 || *t == '#' || *t == ';')) {
                /* sekalian tangkap "# repobase = " bila ada */
                if (memcmp(t, "# repobase = ", 13) == 0) {
                    egg_copy_str(t + 13, g_repo_base, EGG_URL_MAX);
                    char* nl = g_repo_base;
                    while (*nl && *nl != '\n') nl++;
                    *nl = 0;
                    egg_trim(g_repo_base);
                }
                continue;
            }

            int wl = 0;
            while (t[wl]) wl++;
            if (al + wl + 2 >= EGG_ACC_SIZE) { done = 1; }
            else {
                for (int i = 0; i < wl; i++) acc[al++] = t[i];
                acc[al++] = ' ';
                acc[al] = 0;
            }

            /* analisis status entri */
            have_eq = 0; have_open = 0;
            for (int i = 0; i < al; i++) {
                if (acc[i] == '=') have_eq = 1;
                if (acc[i] == '[') have_open = 1;
            }
            if (!have_eq) {
                /* baris asing tanpa '=' (mis. section [bash] dari
                 * index tercampur) — buang seluruh aksumulasi */
                if (acc[0] == '[') { al = 0; acc[0] = 0; }
                continue;
            }
            if (have_open) {
                /* selesai bila ']' muncul SETELAH '[' */
                int open = 0;
                for (int i = 0; i < al; i++) {
                    if (acc[i] == '[') open = 1;
                    if (acc[i] == ']' && open) { done = 1; break; }
                }
            } else {
                done = 1;                     /* nilai tunggal 1 baris */
            }
            if (!*p) done = 1;
        }

        if (!al) continue;
        acc[al] = 0;

        /* nama = token sebelum '=' */
        int eqpos = -1;
        for (int i = 0; i < al; i++)
            if (acc[i] == '=') { eqpos = i; break; }
        if (eqpos <= 0) continue;
        acc[eqpos] = 0;
        char* name = acc;
        egg_trim(name);
        if (!name[0]) continue;
        char* rest = acc + eqpos + 1;

        struct egg_entry* e = &g_pkg[g_npkg];
        for (int i = 0; name[i] && i < 38; i++) e->name[i] = name[i];
        e->name[38] = 0;
        e->version[0] = 0; e->desc[0] = 0; e->sha[0] = 0;
        e->size = 0; e->url[0] = 0; e->nmirror = 0; e->deps[0] = 0;
        e->nsrc = 0;

        /* pecah URL per koma (toleran kutip/spasi/koma ganda dan
         * bracket [ ] dari format list) */
        char* q = rest;
        while (*q && e->nsrc < EGG_MAX_SRC) {
            while (*q == ' ' || *q == '\t' || *q == ',' ||
                   *q == '"' || *q == '\'' || *q == '[' ||
                   *q == ']') q++;
            if (!*q) break;
            char url[EGG_URL_MAX];
            int  ul = 0;
            while (*q && *q != ',' && ul < EGG_URL_MAX - 1)
                url[ul++] = *q++;
            url[ul] = 0;
            while (ul > 0 && (url[ul - 1] == '"' || url[ul - 1] == '\'' ||
                              url[ul - 1] == ' ' || url[ul - 1] == '\t' ||
                              url[ul - 1] == ']' || url[ul - 1] == '['))
                url[--ul] = 0;
            if (!ul) continue;

            char raw[EGG_URL_MAX];
            egg_url_to_raw(url, raw, EGG_URL_MAX);
            for (int i = 0; raw[i] && i < EGG_URL_MAX - 1; i++)
                e->src[e->nsrc][i] = raw[i];
            e->src[e->nsrc][EGG_URL_MAX - 1] = 0;
            e->nsrc++;
        }
        if (e->nsrc > 0) g_npkg++;
    }
}

/* ============================================================
 *  Parser index.idx (v1) — INI per paket + merge ke g_pkg
 * ============================================================ */
static void egg_parse_index(const char* text) {
    g_index_rev[0] = 0;

    const char* p = text;
    char section[40];
    section[0] = 0;
    struct egg_entry* e = 0;

    while (*p) {
        char line[320];
        int  ll = 0;
        while (*p && *p != '\n' && ll < 318) line[ll++] = *p++;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        line[ll] = 0;

        char* t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == 0 || *t == '#' || *t == ';') continue;

        if (*t == '[') {
            char* end = t;
            while (*end && *end != ']') end++;
            if (!*end) continue;
            *end = 0;
            char* s = t + 1;
            egg_trim(s);
            int n = 0;
            while (s[n] && n < 38) { section[n] = s[n]; n++; }
            section[n] = 0;
            e = 0;
            if (!egg_ieq(section, "meta")) {
                for (int i = 0; i < g_npkg; i++)
                    if (egg_ieq(g_pkg[i].name, section)) {
                        e = &g_pkg[i];
                        break;
                    }
            }
            continue;
        }

        char* eq = t;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        *eq = 0;
        char* key = t;
        char* val = eq + 1;
        egg_trim(key);
        egg_trim(val);

        if (egg_ieq(section, "meta")) {
            if (egg_ieq(key, "revision")) {
                int n = 0;
                while (val[n] && n < 22) { g_index_rev[n] = val[n]; n++; }
                g_index_rev[n] = 0;
            }
            continue;
        }
        if (!e) continue;

        if (egg_ieq(key, "version")) {
            int n = 0;
            while (val[n] && n < 18) { e->version[n] = val[n]; n++; }
            e->version[n] = 0;
        } else if (egg_ieq(key, "desc")) {
            int n = 0;
            while (val[n] && n < 94) { e->desc[n] = val[n]; n++; }
            e->desc[n] = 0;
        } else if (egg_ieq(key, "sha256")) {
            int n = 0;
            while (val[n] && n < 70) { e->sha[n] = val[n]; n++; }
            e->sha[n] = 0;
        } else if (egg_ieq(key, "size")) {
            uint32_t v = 0;
            for (int i = 0; val[i] >= '0' && val[i] <= '9'; i++)
                v = v * 10 + (uint32_t)(val[i] - '0');
            e->size = v;
        } else if (egg_ieq(key, "url")) {
            int n = 0;
            while (val[n] && n < EGG_URL_MAX - 1) {
                e->url[n] = val[n]; n++;
            }
            e->url[n] = 0;
        } else if (egg_ieq(key, "mirror")) {
            /* daftar dipisah koma (usulan user: ["url1","url2"]) */
            const char* m = val;
            while (*m && e->nmirror < EGG_MAX_MIRROR) {
                while (*m == ' ' || *m == '"' || *m == '\'') m++;
                char one[EGG_URL_MAX];
                int  ol = 0;
                while (*m && *m != ',' && ol < EGG_URL_MAX - 1)
                    one[ol++] = *m++;
                while (ol > 0 && (one[ol - 1] == '"' ||
                                  one[ol - 1] == ' '))
                    one[--ol] = 0;
                if (ol) {
                    for (int i = 0; i < ol; i++)
                        e->mirror[e->nmirror][i] = one[i];
                    e->mirror[e->nmirror][ol] = 0;
                    e->nmirror++;
                }
                if (*m == ',') m++;
            }
        } else if (egg_ieq(key, "deps")) {
            int n = 0;
            while (val[n] && n < 94) { e->deps[n] = val[n]; n++; }
            e->deps[n] = 0;
        }
    }
}

/* ============================================================
 *  Ambil satu URL/path -> buffer. Path lokal ('/...') dibaca
 *  dari FS; URL via net_eggkg_fetch; gagal -> coba mirror.
 * ============================================================ */
struct egg_progress_ud {
    const char* label;
    uint32_t    total;
};

static void egg_fetch_progress(uint32_t got, void* ud) {
    struct egg_progress_ud* u = (struct egg_progress_ud*)ud;
    egg_bar(u->label, got, u->total);
}

static int egg_read_local(const char* path, uint8_t* buf, uint32_t cap,
                          uint32_t* out_len) {
    struct fs_node* n = fs_get_node_from_path(fs_get_root(), path);
    if (!n || n->is_dir) return -1;
    if (fs_ensure_content(n) != 0) return -1;
    if (!n->content || n->size == 0) return -1;
    uint32_t len = n->size;
    if (len > cap - 1) return -1;             /* terlalu besar utk buf */
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)n->content[i];
    buf[len] = 0;
    *out_len = len;
    return 0;
}

static int egg_fetch_one(const char* url, uint8_t* buf, uint32_t cap,
                         uint32_t* out_len) {
    if (url[0] == '/')
        return egg_read_local(url, buf, cap, out_len);

    struct egg_progress_ud ud;
    ud.label = "        mengunduh";
    ud.total = 0;                             /* tak tentu -> spinner */
    int status = 0;
    uint32_t len = 0;
    if (net_eggkg_fetch(url, buf, cap, &len, &status,
                        egg_fetch_progress, &ud) != 0) {
        egg_line_clear();
        return -1;
    }
    egg_line_clear();
    *out_len = len;
    return 0;
}

/* fetch dengan mirror fallback (index v1) */
static int egg_fetch_with_mirror(struct egg_entry* e, const char* url,
                                 uint8_t* buf, uint32_t cap,
                                 uint32_t* out_len) {
    if (egg_fetch_one(url, buf, cap, out_len) == 0) return 0;
    for (int i = 0; i < e->nmirror; i++) {
        printf("      mirror %d: %s\n", i + 1, e->mirror[i]);
        if (egg_fetch_one(e->mirror[i], buf, cap, out_len) == 0) return 0;
    }
    return -1;
}

/* ============================================================
 *  FS helper: mkdir -p, tulis file, split path
 * ============================================================ */
static int egg_mkdir_p(const char* path) {
    char buf[128];
    int n = 0;
    while (path[n] && n < 126) { buf[n] = path[n]; n++; }
    buf[n] = 0;

    struct fs_node* cur = fs_get_root();
    int i = 0;
    while (buf[i]) {
        if (buf[i] == '/') { i++; continue; }
        char seg[64];
        int sl = 0;
        while (buf[i] && buf[i] != '/' && sl < 62) seg[sl++] = buf[i++];
        seg[sl] = 0;
        struct fs_node* nx = fs_find_child(cur, seg);
        if (!nx) {
            if (fs_create_dir(cur, seg) != 0) return -1;
            nx = fs_find_child(cur, seg);
            if (!nx) return -1;
        }
        if (!nx->is_dir) return -1;
        cur = nx;
    }
    return (cur != fs_get_root()) ? 0 : -1;
}

static int egg_write_file(const char* path, const uint8_t* data,
                          uint32_t len) {
    char dir[160];
    int  n = 0;
    while (path[n] && n < 158) { dir[n] = path[n]; n++; }
    dir[n] = 0;
    int last = -1;
    for (int i = 0; dir[i]; i++)
        if (dir[i] == '/') last = i;
    if (last < 0) return -1;
    dir[last] = 0;
    const char* fname = dir + last + 1;

    /* parent dibuat bila belum ada (mkdir -p) */
    if (egg_mkdir_p(dir) != 0) return -1;
    struct fs_node* parent = fs_get_node_from_path(fs_get_root(), dir);
    if (!parent || !parent->is_dir) return -1;
    return fs_write_binary(parent, fname, data, len);
}

static int egg_ends_with(const char* s, const char* suf) {
    int ls = 0, lf = 0;
    while (s[ls]) ls++;
    while (suf[lf]) lf++;
    if (lf > ls) return 0;
    for (int i = 0; i < lf; i++)
        if (s[ls - lf + i] != suf[i]) return 0;
    return 1;
}

/* ============================================================
 *  Direktori lokal + server (ecf [eggkg] dengan fallback)
 * ============================================================ */
static void egg_copy_str(const char* s, char* out, int outmax) {
    int n = 0;
    for (; s[n] && n < outmax - 1; n++) out[n] = s[n];
    out[n] = 0;
}

static void egg_local_dir(char* out, int outmax) {
    const char* act = ecf_active_path(0);
    if (act && act[0]) {
        const char* v = ecf_file_get(act, "eggkg.local");
        if (v && v[0]) { egg_copy_str(v, out, outmax); return; }
    }
    egg_copy_str(EGG_LOCAL_DEFAULT, out, outmax);
}

static void egg_server_url(char* out, int outmax) {
    const char* act = ecf_active_path(0);
    if (act && act[0]) {
        const char* v = ecf_file_get(act, "eggkg.server");
        if (v && v[0]) { egg_copy_str(v, out, outmax); return; }
    }
    egg_copy_str(EGG_SERVER_DEFAULT, out, outmax);
}

/* ============================================================
 *  installed.db — INI mini (parser & serializer sendiri; value
 *  "files" bisa melebihi batas 127 char ecf, jadi tidak memakai
 *  parser ecf).
 *      [meta]
 *      format = 1
 *      [bash]
 *      version = 0.1.0
 *      files   = 19
 *      f01 = /bin/cat.mrp
 *      deps =
 * ============================================================ */
static struct egg_installed* g_inst;    /* EGG_MAX_PKG entri           */
static int                   g_ninst;

static int egg_ensure_installed(void) {
    if (!g_inst) {
        g_inst = (struct egg_installed*)malloc(
            sizeof(struct egg_installed) * EGG_MAX_PKG);
        if (!g_inst) return -1;
        memset(g_inst, 0, sizeof(struct egg_installed) * EGG_MAX_PKG);
    }
    return 0;
}

static void db_reset(void) {
    if (egg_ensure_installed() != 0) return;
    g_ninst = 0;
}

static struct egg_installed* db_find(const char* name) {
    for (int i = 0; i < g_ninst; i++)
        if (egg_ieq(g_inst[i].name, name)) return &g_inst[i];
    return 0;
}

static struct egg_installed* db_add(const char* name) {
    if (g_ninst >= EGG_MAX_PKG) return 0;
    struct egg_installed* e = &g_inst[g_ninst++];
    for (int i = 0; i < (int)sizeof(e->files) / 48; i++) e->files[i][0] = 0;
    e->nfiles = 0;
    e->version[0] = 0;
    e->deps[0] = 0;
    int n = 0;
    for (; name[n] && n < 38; n++) e->name[n] = name[n];
    e->name[n] = 0;
    return e;
}

static void db_load(void) {
    if (egg_ensure_installed() != 0) return;
    db_reset();
    char local[128];
    char p[160];
    egg_local_dir(local, sizeof(local));
    snprintf(p, sizeof(p), "%s/%s", local, EGG_DB_NAME);

    static uint8_t buf[EGG_DB_MAX];
    uint32_t len = 0;
    if (egg_read_local(p, buf, EGG_DB_MAX, &len) != 0) return;
    buf[len] = 0;

    struct egg_installed* cur = 0;
    const char* s = (const char*)buf;
    while (*s) {
        char line[256];
        int  ll = 0;
        while (*s && *s != '\n' && ll < 254) line[ll++] = *s++;
        while (*s && *s != '\n') s++;
        if (*s == '\n') s++;
        line[ll] = 0;

        char* t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == 0 || *t == '#' || *t == ';') continue;

        if (*t == '[') {
            char* end = t;
            while (*end && *end != ']') end++;
            *end = 0;
            char* nm = t + 1;
            egg_trim(nm);
            if (egg_ieq(nm, "meta")) { cur = 0; continue; }
            cur = db_find(nm);
            if (!cur) cur = db_add(nm);
            continue;
        }

        char* eq = t;
        while (*eq && *eq != '=') eq++;
        if (!*eq || !cur) continue;
        *eq = 0;
        char* key = t;
        char* val = eq + 1;
        egg_trim(key);
        egg_trim(val);

        if (egg_ieq(key, "version")) {
            int n = 0;
            while (val[n] && n < 18) { cur->version[n] = val[n]; n++; }
            cur->version[n] = 0;
        } else if (egg_ieq(key, "deps")) {
            int n = 0;
            while (val[n] && n < 94) { cur->deps[n] = val[n]; n++; }
            cur->deps[n] = 0;
        } else if ((key[0] == 'f' || key[0] == 'F') && key[1] >= '0' &&
                   key[1] <= '9' && cur->nfiles < EGG_MAX_FILES) {
            int n = 0;
            while (val[n] && n < 46) {
                cur->files[cur->nfiles][n] = val[n]; n++;
            }
            cur->files[cur->nfiles][n] = 0;
            cur->nfiles++;
        }
    }
}

static void db_save(void) {
    if (!g_inst) return;
    static uint8_t buf[EGG_DB_MAX];
    uint32_t o = 0;
    const char* head = "# eggkg installed.db — jangan edit tangan\n"
                       "[meta]\nformat = 1\n";
    for (const char* s = head; *s && o < EGG_DB_MAX - 1; s++)
        buf[o++] = (uint8_t)*s;

    for (int i = 0; i < g_ninst; i++) {
        struct egg_installed* e = &g_inst[i];
        o += (uint32_t)snprintf((char*)buf + o, EGG_DB_MAX - o,
                                "[%s]\nversion = %s\ndeps = %s\nfiles = %d\n",
                                e->name,
                                e->version[0] ? e->version : "0",
                                e->deps, e->nfiles);
        if (o >= EGG_DB_MAX - 1) break;
        for (int f = 0; f < e->nfiles && o < EGG_DB_MAX - 8; f++)
            o += (uint32_t)snprintf((char*)buf + o, EGG_DB_MAX - o,
                                    "f%02d = %s\n", f + 1, e->files[f]);
        if (o >= EGG_DB_MAX - 1) break;
        if (o < EGG_DB_MAX - 1) buf[o++] = '\n';
    }
    buf[o] = 0;

    char local[128];
    char p[160];
    egg_local_dir(local, sizeof(local));
    snprintf(p, sizeof(p), "%s/%s", local, EGG_DB_NAME);
    if (egg_write_file(p, buf, o) != 0)
        printf("eggkg: gagal menulis %s\n", p);
}

static int db_dep_exists(const char* name, char* who, int wholen) {
    for (int i = 0; i < g_ninst; i++) {
        if (egg_ieq(g_inst[i].name, name)) continue;
        /* deps = "a, b, c" — cari token persis */
        const char* d = g_inst[i].deps;
        while (*d) {
            while (*d == ' ' || *d == ',') d++;
            char tok[48];
            int  tl = 0;
            while (*d && *d != ',' && tl < 46) tok[tl++] = *d++;
            while (tl > 0 && tok[tl - 1] == ' ') tl--;
            tok[tl] = 0;
            if (tl && egg_ieq(tok, name)) {
                int n = 0;
                for (; g_inst[i].name[n] && n < wholen - 1; n++)
                    who[n] = g_inst[i].name[n];
                who[n] = 0;
                return 1;
            }
        }
    }
    return 0;
}

/* ============================================================
 *  [dependencies] di system.ecf (permintaan user — header baru)
 *  - key sudah ada  -> patch via ecf_set_file
 *  - key belum ada  -> append cantik:
 *         [dependencies]
 *         bash = true
 * ============================================================ */
static uint8_t egg_ecf_buf[ECF_FILE_MAX + 64];

static int egg_ecf_set_dependencies(int on) {
    const char* act = ecf_active_path(1);
    if (!act || !act[0]) {
        printf("      system.ecf aktif tidak ada (live ISO?) — "
               "settings dilewati\n");
        return -1;
    }
    const char* cur = ecf_file_get(act, "dependencies.bash");
    if (cur && cur[0]) {
        if (egg_ieq(cur, on ? "true" : "false")) return 0;
        int rc = ecf_set_file(act, "dependencies.bash",
                              on ? "true" : "false");
        return (rc == 0) ? 0 : -1;
    }

    /* append dengan header seksi. Bila file belum ada (live ISO,
     * /boot baru dibuat ecf_active_path) -> buat system.ecf minimal
     * berisi blok [dependencies] (session-only, pola ecf 0.4 Beta). */
    struct fs_node* n = fs_get_node_from_path(fs_get_root(), act);
    uint32_t len = 0;
    if (n && !n->is_dir) {
        if (n->size && fs_ensure_content(n) == 0 && n->content) {
            if (n->size > ECF_FILE_MAX) return -1;
            for (uint32_t i = 0; i < n->size; i++)
                egg_ecf_buf[i] = (uint8_t)n->content[i];
            len = n->size;
        }
    }
    if (len > 0 && egg_ecf_buf[len - 1] != '\n')
        egg_ecf_buf[len++] = '\n';
    const char* block = on ? "[dependencies]\nbash = true\n"
                           : "[dependencies]\nbash = false\n";
    for (const char* s = block; *s; s++)
        egg_ecf_buf[len++] = (uint8_t)*s;

    if (!n || n->is_dir) {
        /* file baru: tulis lewat direktori induk */
        char dir[160];
        egg_copy_str(act, dir, (int)sizeof(dir));
        int last = -1;
        for (int i = 0; dir[i]; i++)
            if (dir[i] == '/') last = i;
        if (last <= 0) return -1;
        dir[last] = 0;
        if (egg_mkdir_p(dir) != 0) return -1;
        struct fs_node* parent =
            fs_get_node_from_path(fs_get_root(), dir);
        if (!parent || !parent->is_dir) return -1;
        return fs_write_binary(parent, dir + last + 1, egg_ecf_buf,
                               len);
    }
    return fs_write_binary(n->parent, n->name, egg_ecf_buf, len);
}

/* ============================================================
 *  Format .ruf v2 — ekstraksi [src:*] + penurunan resep v1
 * ============================================================ */
static int egg_v2_extract(const char* body, const char* srcdir) {
    int count = 0;
    const char* p = body;
    while (*p) {
        char line[400];
        int  ll = 0;
        while (*p && *p != '\n' && ll < 398) line[ll++] = *p++;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        line[ll] = 0;

        if (line[0] != '[') continue;
        char* end = line;
        while (*end && *end != ']') end++;
        if (!*end) continue;
        *end = 0;
        char* hdr = line + 1;
        if (memcmp(hdr, "src:", 4) != 0) continue;
        char* fname = hdr + 4;
        egg_trim(fname);
        if (!fname[0]) continue;

        /* lewati baris pembuka heredoc (<<EGGKG_SRC_...) */
        char content[ECF_FILE_MAX];
        uint32_t cl = 0;
        int closed = 0, opened = 0;
        while (*p) {
            int  wl = 0;
            while (p[wl] && p[wl] != '\n' && wl < 398) wl++;
            int has_nl = (p[wl] == '\n');
            if (memcmp(p, "<<", 2) == 0) {
                if (!opened) opened = 1;     /* pembuka              */
                else { closed = 1; }         /* penutup              */
                p += wl;
                if (has_nl) p++;
                if (closed) break;
                continue;
            }
            if (opened && cl + (uint32_t)wl + 2 < sizeof(content)) {
                for (int i = 0; i < wl; i++) content[cl++] = p[i];
                content[cl++] = '\n';
            }
            p += wl;
            if (has_nl) p++;
        }
        if (!closed) return -1;

        char fp[160];
        snprintf(fp, sizeof(fp), "%s/%s", srcdir, fname);
        if (egg_write_file(fp, (const uint8_t*)content, cl) != 0)
            return -1;
        count++;
    }
    return count;
}

/* turunkan resep v1 murni dari teks v1/v2: hanya baris kosong,
 * komentar, dan key milik mtcc (echo/src/exclude/out/lib) */
static int egg_v1_recipe(const char* body, char* out, int outmax) {
    int o = 0;
    const char* p = body;
    int skip_heredoc = 0, marks = 0;
    while (*p) {
        char line[400];
        int  ll = 0;
        while (*p && *p != '\n' && ll < 398) line[ll++] = *p++;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        line[ll] = 0;

        char* t = line;
        while (*t == ' ' || *t == '\t') t++;

        if (skip_heredoc) {
            if (t[0] == '<' && t[1] == '<') {
                marks++;
                if (marks >= 2) { skip_heredoc = 0; marks = 0; }
            }
            continue;
        }
        if (t[0] == '[' && memcmp(t, "[src:", 5) == 0) {
            skip_heredoc = 1;
            marks = 0;
            continue;
        }
        if (t[0] == 0 || t[0] == '#') {
            for (const char* s = t; *s && o < outmax - 2; s++)
                out[o++] = *s;
            out[o++] = '\n';
            continue;
        }
        if (memcmp(t, "echo", 4) == 0 || memcmp(t, "src", 3) == 0 ||
            memcmp(t, "exclude", 7) == 0 || memcmp(t, "out", 3) == 0 ||
            memcmp(t, "lib", 3) == 0 ||
            memcmp(t, "copy", 4) == 0 || memcmp(t, "move", 5) == 0 ||
            strstr(t, ":=") != NULL) {
            for (const char* s = t; *s && o < outmax - 2; s++)
                out[o++] = *s;
            out[o++] = '\n';
        }
        /* Everything else goes: the [package] section header and the
           plain `key = value` metadata of .ruf v2. The v3 assignment
           `name := "value"` is kept because that is a make directive
           mtcc understands — dropping it leaves every $name literal
           in the recipe, so `src $source` ends up looking for a
           directory literally named '$source' and the build dies
           with 0 jobs. copy/move (post-build actions) are directives
           too and must survive the filter. */
    }
    out[o] = 0;
    return o;
}

/* ============================================================
 *  Muat repositori LOKAL (hasil eggkg update) ke g_pkg
 * ============================================================ */
static char g_list_path[160];

static int egg_load_local_index(void) {
    char local[128];
    egg_local_dir(local, sizeof(local));

    snprintf(g_list_path, sizeof(g_list_path), "%s/%s", local,
             EGG_LIST_NAME);

    uint8_t* buf = (uint8_t*)malloc(EGG_NET_CAP);
    if (!buf) return -1;
    uint32_t len = 0;
    if (egg_read_local(g_list_path, buf, EGG_NET_CAP, &len) != 0) {
        free(buf);
        return -1;
    }
    buf[len] = 0;

    /* repobase dari komentar "# repobase = ..." yang ditulis update */
    g_repo_base[0] = 0;
    const char* s = (const char*)buf;
    while (*s) {
        if (memcmp(s, "# repobase = ", 13) == 0) {
            egg_copy_str(s + 13, g_repo_base, EGG_URL_MAX);
            char* nl = g_repo_base;
            while (*nl && *nl != '\n') nl++;
            *nl = 0;
            egg_trim(g_repo_base);
            break;
        }
        while (*s && *s != '\n') s++;
        if (*s == '\n') s++;
    }

    egg_parse_list((const char*)buf);

    /* index.idx lokal (opsional) */
    snprintf(g_list_path, sizeof(g_list_path), "%s/%s", local,
             EGG_INDEX_NAME);
    len = 0;
    if (egg_read_local(g_list_path, buf, EGG_NET_CAP, &len) == 0) {
        buf[len] = 0;
        egg_parse_index((const char*)buf);
    }
    free(buf);
    return 0;
}

/* ============================================================
 *  eggkg update [sumber]
 * ============================================================ */
static void egg_cmd_update(const char* src_arg) {
    char src[EGG_URL_MAX];
    if (src_arg && src_arg[0])
        egg_copy_str(src_arg, src, EGG_URL_MAX);
    else
        egg_server_url(src, EGG_URL_MAX);

    uint8_t* buf = (uint8_t*)malloc(EGG_NET_CAP);
    if (!buf) {
        printf("eggkg: heap kernel habis (butuh %d KB)\n",
               EGG_NET_CAP / 1024);
        return;
    }

    char local[128];
    egg_local_dir(local, sizeof(local));
    egg_mkdir_p(local);

    printf("eggkg: update (server: %s)\n", src);
    egg_stage(1, 2, "mengunduh package.list");

    uint32_t len = 0;
    if (egg_fetch_one(src, buf, EGG_FETCH_CAP, &len) != 0) {
        egg_fail("server tidak terjangkau / file tidak ada");
        printf("      sumber: %s\n", src);
        printf("      cek: ifconfig (net up?), path benar, atau\n");
        printf("           eggkg update /equinox/repo/package.list\n");
        free(buf);
        return;
    }
    buf[len] = 0;
    if (len < 3 || buf[0] == '<') {
        /* halaman HTML / kosong = format salah */
        egg_fail("isi bukan package.list (HTML/kosong?)");
        free(buf);
        return;
    }
    printf("      [ok] %u bytes\n", (unsigned)len);

    egg_parse_list((const char*)buf);
    if (g_npkg == 0) {
        egg_fail("format package.list tidak dikenali");
        printf("      format: nama = [\"url1\", \"url2\", ...]\n");
        free(buf);
        return;
    }

    /* derive repobase dari sumber LIST (bukan dari URL .c) */
    egg_repo_base_from(src, g_repo_base, EGG_URL_MAX);

    /* simpan + tulis komentar repobase utk eggkg install — ukuran
     * PAS (len + komentar); heap live-ISO sempit, jangan boros */
    uint32_t ocap = len + 128;
    uint8_t* out = (uint8_t*)malloc(ocap);
    if (!out) {
        printf("eggkg: heap habis (save package.list)\n");
        free(buf);
        return;
    }
    uint32_t o = 0;
    o += (uint32_t)snprintf((char*)out, ocap - 1,
                            "# repobase = %s\n", g_repo_base);
    for (uint32_t i = 0; i < len && o < ocap - 1; i++)
        out[o++] = buf[i];
    out[o] = 0;
    char plist[160];
    snprintf(plist, sizeof(plist), "%s/%s", local, EGG_LIST_NAME);
    if (egg_write_file(plist, out, o) != 0)
        printf("      [gagal] tulis %s\n", plist);
    free(out);

    egg_stage(2, 2, "index.idx (opsional)");
    char iurl[EGG_URL_MAX];
    snprintf(iurl, sizeof(iurl), "%s%s", g_repo_base, EGG_INDEX_NAME);
    len = 0;
    if (egg_fetch_one(iurl, buf, EGG_FETCH_CAP, &len) == 0) {
        buf[len] = 0;
        egg_parse_index((const char*)buf);
        char ipath[160];
        snprintf(ipath, sizeof(ipath), "%s/%s", local, EGG_INDEX_NAME);
        egg_write_file(ipath, buf, len);
        if (g_index_rev[0])
            printf("      [ok] revision %s\n", g_index_rev);
        else
            printf("      [ok]\n");
    } else {
        printf("      (tanpa index.idx — verifikasi sha256 "
               "dilewati, package.list v0 tetap sah)\n");
    }

    int with_ver = 0;
    for (int i = 0; i < g_npkg; i++)
        if (g_pkg[i].version[0]) with_ver++;
    printf("eggkg: %d paket di index%s\n", g_npkg,
           with_ver ? "" : " (package.list v0 — tanpa versi/hash)");
    free(out);
    free(buf);
}

/* ============================================================
 *  eggkg install <nama> [-y]
 * ============================================================ */
static void egg_cmd_install(const char* name, int assume_yes) {
    char local[128];
    egg_local_dir(local, sizeof(local));

    if (egg_load_local_index() != 0) {
        printf("eggkg: index lokal belum ada.\n");
        printf("      jalan dulu: eggkg update  (atau: eggkg update "
               "/equinox/repo/package.list)\n");
        return;
    }

    struct egg_entry* e = 0;
    for (int i = 0; i < g_npkg; i++)
        if (egg_ieq(g_pkg[i].name, name)) { e = &g_pkg[i]; break; }
    if (!e) {
        printf("eggkg: paket '%s' tidak ada di index (%d paket).\n",
               name, g_npkg);
        printf("      cari:  eggkg search <pola>\n");
        return;
    }

    db_load();

    printf("eggkg: install %s%s%s\n", name,
           e->version[0] ? " " : "", e->version[0] ? e->version : "");

    uint8_t* buf = NULL;      /* dialokasi per mode (lihat [2/4]) */
    uint32_t len = 0;
    int rc;

    /* resep build — bisa datang dari 3 sumber (urut prioritas):
     * 1. entri .ruf di package.list (repo user memasang build.ruf
     *    sebagai anggota daftar sumber)
     * 2. unduhan <repobase>/<pkg>/build.ruf
     * 3. arsip .ruf v2 / sintesis kanonik */
    static char recipe[8192];
    int  have_recipe = 0;

    /* ---------- [1/4] rencana ---------- */
    egg_stage(1, 4, "membaca index");
    {
        char note[96];
        if (e->url[0])
            snprintf(note, sizeof(note), "arsip paket (%d mirror)",
                     e->nmirror);
        else
            snprintf(note, sizeof(note), "%d sumber .c", e->nsrc);
        egg_ok(note);
        if (!e->url[0] && !e->sha[0])
            printf("      (package.list v0 — tanpa sha256, "
                   "verifikasi dilewati)\n");
    }

    char pkgdir[128];
    char srcdir[160];
    snprintf(pkgdir, sizeof(pkgdir), "%s/%s", local, e->name);
    snprintf(srcdir, sizeof(srcdir), "%s/src", pkgdir);
    if (egg_mkdir_p(srcdir) != 0) {
        egg_fail("buat folder .local gagal");
        free(buf);
        return;
    }

    /* ---------- [2/4] unduh ---------- */
    int n_extracted = 0;
    egg_stage(2, 4, e->url[0] ? "mengunduh arsip paket"
                              : "mengunduh sumber");
    if (e->url[0]) {
        /* --- mode arsip (index v1 / .ruf v2) --- */
        buf = (uint8_t*)malloc(EGG_FETCH_CAP);
        if (!buf) {
            printf("eggkg: heap kernel habis (arsip)\n");
            return;
        }
        char cname[96];
        snprintf(cname, sizeof(cname), "%s/cache/%s%s%s.ruf", local,
                 e->name, e->version[0] ? "-" : "",
                 e->version[0] ? e->version : "");
        if (egg_fetch_with_mirror(e, e->url, buf, EGG_FETCH_CAP,
                                  &len) != 0) {
            egg_fail("unduhan arsip gagal (primer + mirror)");
            free(buf);
            return;
        }
        if (egg_write_file(cname, buf, len) != 0)
            printf("      (cache %s gagal — lanjut)\n", cname);
        printf("      [ok] %u bytes\n", (unsigned)len);

        if (e->size && e->size != len) {
            egg_fail("size tidak cocok");
            printf("      harus %u, dapat %u\n",
                   (unsigned)e->size, (unsigned)len);
            free(buf);
            return;
        }
        if (e->sha[0]) {
            char hex[65];
            egg_sha256_hex(buf, len, hex);
            if (!egg_sha_eq(hex, e->sha)) {
                char h16[17], d16[17];
                egg_hex16(e->sha, h16);
                egg_hex16(hex, d16);
                egg_fail("sha256 tidak cocok");
                printf("      harus  %s...\n", h16);
                printf("      dapat  %s...\n", d16);
                free(buf);
                return;
            }
            char s16[17];
            egg_hex16(hex, s16);
            printf("      [ok] sha256 terverifikasi (%s...)\n", s16);
        }

        buf[len] = 0;
        n_extracted = egg_v2_extract((const char*)buf, srcdir);
        if (n_extracted < 0) {
            egg_fail("ekstraksi [src:*] rusak (terminator hilang)");
            free(buf);
            return;
        }
        if (n_extracted > 0)
            printf("      [ok] %d sumber diekstrak ke src/\n",
                   n_extracted);
    } else {
        /* --- mode sumber lepas (package.list v0) --- */
        buf = (uint8_t*)malloc(EGG_NET_CAP);
        if (!buf) {
            printf("eggkg: heap kernel habis (sumber)\n");
            return;
        }
        for (int i = 0; i < e->nsrc; i++) {
            const char* su = e->src[i];
            const char* bn = su;
            for (const char* q = su; *q; q++)
                if (*q == '/') bn = q + 1;

            char label[96];
            snprintf(label, sizeof(label), "      [%d/%d] %s",
                     i + 1, e->nsrc, bn);
            struct egg_progress_ud ud;
            ud.label = label;
            ud.total = 0;

            if (su[0] == '/') {
                if (egg_read_local(su, buf, EGG_NET_CAP, &len) != 0) {
                    printf("%s [gagal] (tidak ada)\n", label);
                    free(buf);
                    return;
                }
            } else {
                int status = 0;
                if (net_eggkg_fetch(su, buf, EGG_NET_CAP, &len,
                                    &status, egg_fetch_progress,
                                    &ud) != 0) {
                    egg_line_clear();
                    printf("%s [gagal] (jaringan)\n", label);
                    free(buf);
                    return;
                }
                egg_line_clear();
            }

            if (egg_ends_with(bn, ".ruf")) {
                /* entri resep (mis. build.ruf di repo Eggkg-l) —
                 * BUKAN sumber: turunkan ke v1 & simpan utk [3/4] */
                buf[len] = 0;             /* WAJIB: parser C-string */
                egg_v1_recipe((const char*)buf, recipe,
                              (int)sizeof(recipe) - 1);
                if (recipe[0]) have_recipe = 1;
                printf("%s [ok] (resep, %u B)\n", label,
                       (unsigned)len);
                continue;
            }

            char fp[192];
            snprintf(fp, sizeof(fp), "%s/%s", srcdir, bn);
            if (egg_write_file(fp, buf, len) != 0) {
                printf("%s [gagal] (tulis)\n", label);
                free(buf);
                return;
            }
            printf("%s [ok] (%u B)\n", label, (unsigned)len);
        }
        /* mode sumber: buf tak dipakai lagi — BEBASKAN sebelum build
         * supaya mtcc (SYS_MKFILE -> malloc kernel heap) tidak kehabisan
         * memori saat menulis 19 file .mrp (lesson 0.4 Beta: 7 "GAGAL
         * tulis" karena heap penuh). */
        free(buf);
        buf = NULL;
    }

    /* ---------- [3/4] build ---------- */
    egg_stage(3, 4, "build (mtcc -make)");
    char rufpath[192];
    snprintf(rufpath, sizeof(rufpath), "%s/build.ruf", pkgdir);

    /* sumber 2: unduhan langsung <repobase>/<pkg>/build.ruf */
    if (!have_recipe && g_repo_base[0]) {
        char rurl[EGG_URL_MAX];
        snprintf(rurl, sizeof(rurl), "%s%s/build.ruf", g_repo_base,
                 e->name);
        if (!buf) {
            buf = (uint8_t*)malloc(EGG_NET_CAP);
            if (!buf) {
                printf("eggkg: heap habis (resep)\n");
                return;
            }
        }
        len = 0;
        if (egg_fetch_one(rurl, buf, EGG_NET_CAP, &len) == 0) {
            buf[len] = 0;
            if (len > 2 && buf[0] != '<') {       /* bukan HTML 404 */
                egg_v1_recipe((const char*)buf, recipe,
                              (int)sizeof(recipe) - 1);
                have_recipe = (recipe[0] != 0);
            }
        }
    }
    if (!have_recipe && buf && e->url[0] && n_extracted >= 0 && len > 0) {
        /* buf masih berisi arsip bila build.ruf repo 404 —
         * turunkan resep dari arsip */
        egg_v1_recipe((const char*)buf, recipe, (int)sizeof(recipe) - 1);
        have_recipe = (recipe[0] != 0);
    }
    if (buf) { free(buf); buf = NULL; }   /* BEBASKAN sebelum mtcc */
    if (!have_recipe) {
        int nsrc_now = (e->nsrc > 0) ? e->nsrc : n_extracted;
        snprintf(recipe, sizeof(recipe),
                 "# build.ruf — dihasilkan eggkg (paket '%s', server "
                 "tanpa resep)\necho \"eggkg: membangun paket %s\"\n"
                 "src %s/src\nout %s\n",
                 e->name, e->name, pkgdir, pkgdir);
        printf("      (repo tanpa build.ruf — resep kanonik "
               "dihasilkan)\n");
        (void)nsrc_now;
    }
    if (egg_write_file(rufpath, (const uint8_t*)recipe,
                       (uint32_t)strlen(recipe)) != 0) {
        egg_fail("tulis build.ruf gagal");
        free(buf);
        return;
    }
    printf("      resep: %s\n", rufpath);

    rc = shell_eggkg_build_ruf(rufpath);
    if (rc != 0) {
        egg_fail(rc > 0 ? "mtcc melaporkan job gagal"
                        : "spawn mtcc gagal");
        free(buf);
        return;
    }
    egg_ok("build selesai");

    /* ---------- [4/4] pasang ---------- */
    egg_stage(4, 4, "pasang ke /bin + database");
    struct fs_node* bindir = fs_get_node_from_path(fs_get_root(),
                                                   EGG_BIN_DIR);
    if (!bindir) {
        if (egg_mkdir_p(EGG_BIN_DIR) != 0) {
            egg_fail("buat /bin gagal");
            free(buf);
            return;
        }
        bindir = fs_get_node_from_path(fs_get_root(), EGG_BIN_DIR);
        if (!bindir) {
            egg_fail("/bin tidak tersedia");
            free(buf);
            return;
        }
    }

    /* pemikat populate utk volume FAT32 (children lazily diisi) */
    fs_find_child(bindir, ".");

    struct egg_installed* inst = db_find(e->name);
    if (!inst) inst = db_add(e->name);
    if (!inst) {
        egg_fail("installed.db penuh");
        free(buf);
        return;
    }
    inst->nfiles = 0;
    egg_copy_str(e->version, inst->version, sizeof(inst->version));
    egg_copy_str(e->deps, inst->deps, sizeof(inst->deps));

    int nbin = 0;
    /* hasil build ada di <pkgdir> (out) — PINDAHKAN ke /bin.
     * RAMFS: fs_ram_relink_node = pindah node TANPA salin konten
     * (zero-copy; lesson 0.4 Beta: 19 x 33 KB terduplikasi mengoyak heap
     * ~1 MB -> "GAGAL tulis"/fragmentasi). FAT: salin + hapus —
     * konten .mrp tinggal di fat arena, bukan kernel heap. */
    struct fs_node* outdir = fs_get_node_from_path(fs_get_root(),
                                                   pkgdir);
    if (outdir) {
        fs_find_child(outdir, "build.ruf");   /* pemicu populate FAT  */
        for (struct fs_node* c = outdir->children; c; ) {
            struct fs_node* nx = c->next;     /* aman utk relink/del  */
            if (!c->is_dir && egg_ends_with(c->name, ".mrp")) {
                int rc;
                struct fs_node* ex = fs_find_child(bindir, c->name);
                if (ex) fs_delete_node(bindir, c->name);
                if (!c->backing && !bindir->backing) {
                    rc = fs_ram_relink_node(c, bindir, c->name);
                } else if (fs_ensure_content(c) == 0 && c->content) {
                    rc = fs_write_binary(bindir, c->name,
                                         (const uint8_t*)c->content,
                                         c->size);
                    if (rc == 0) fs_delete_node(outdir, c->name);
                    else fs_delete_node(bindir, c->name);
                } else {
                    rc = -3;
                }
                if (rc != 0) {
                    printf("      [gagal] pindah %s\n", c->name);
                } else {
                    if (inst->nfiles < EGG_MAX_FILES) {
                        snprintf(inst->files[inst->nfiles++], 48,
                                 "%s/%s", EGG_BIN_DIR, c->name);
                    }
                    nbin++;
                }
            }
            c = nx;
        }
    }
    if (nbin == 0) {
        egg_fail("tidak ada .mrp dihasilkan");
        free(buf);
        return;
    }
    printf("      [ok] %d perintah di " EGG_BIN_DIR "\n", nbin);
    db_save();

    /* ---------- post-hook [dependencies] (paket bash) ---------- */
    if (egg_ieq(e->name, "bash")) {
        int want = assume_yes;
        char ans[16];
        if (!assume_yes) {
            printf("      tulis [dependencies] bash=true ke "
                   "system.ecf? (y/n) ");
            ans[0] = 0;
            gets(ans, (int)sizeof(ans));
            want = (ans[0] == 'y' || ans[0] == 'Y' ||
                    ans[0] == '1' || ans[0] == 't' || ans[0] == 'T');
        }
        if (egg_ecf_set_dependencies(1) == 0 && want)
            printf("      [ok] [dependencies] bash=true\n");
    }

    printf("\n  %s%s terpasang — %d perintah aktif (system path "
           "/bin).\n", e->name, e->version[0] ? e->version : "", nbin);
    printf("  coba langsung: ls, grep, wc  |  nonaktif: eggkg remove "
           "%s\n", e->name);
    free(buf);
}

/* ============================================================
 *  eggkg remove <nama> [-y]
 * ============================================================ */
static void egg_cmd_remove(const char* name, int assume_yes) {
    db_load();
    struct egg_installed* inst = db_find(name);
    if (!inst) {
        printf("eggkg: '%s' tidak tercatat terpasang (eggkg list)\n",
               name);
        return;
    }

    /* pagar reverse-dependency */
    char who[40];
    if (db_dep_exists(name, who, sizeof(who))) {
        printf("eggkg: tolak — paket '%s' bergantung pada '%s'\n",
               who, name);
        printf("      hapus '%s' dulu.\n", who);
        return;
    }

    struct fs_node* bindir = fs_get_node_from_path(fs_get_root(),
                                                   EGG_BIN_DIR);
    int ndel = 0;
    for (int i = 0; i < inst->nfiles; i++) {
        const char* fp = inst->files[i];
        if (!bindir) break;
        const char* bn = fp;
        for (const char* q = fp; *q; q++)
            if (*q == '/') bn = q + 1;
        struct fs_node* f = fs_find_child(bindir, bn);
        if (f) {
            fs_delete_node(bindir, bn);
            ndel++;
        }
    }
    printf("eggkg: %d file dihapus dari " EGG_BIN_DIR "\n", ndel);

    /* nonaktifkan dependencies.bash bila paket bash dilepas */
    if (egg_ieq(name, "bash")) {
        const char* act = ecf_active_path(0);
        const char* cur = (act && act[0])
                              ? ecf_file_get(act, "dependencies.bash")
                              : 0;
        if (cur && egg_ieq(cur, "true")) {
            int want = assume_yes;
            char ans[16];
            if (!assume_yes) {
                printf("      set [dependencies] bash=false? (y/n) ");
                ans[0] = 0;
                gets(ans, (int)sizeof(ans));
                want = (ans[0] == 'y' || ans[0] == 'Y' ||
                        ans[0] == '1' || ans[0] == 't');
            }
            if (want && egg_ecf_set_dependencies(0) == 0)
                printf("      [ok] [dependencies] bash=false\n");
        }
    }

    /* hapus dari db (geser array) */
    for (int i = 0; i < g_ninst; i++) {
        if (egg_ieq(g_inst[i].name, name)) {
            for (int j = i; j + 1 < g_ninst; j++)
                g_inst[j] = g_inst[j + 1];
            g_ninst--;
            break;
        }
    }
    db_save();
    printf("eggkg: '%s' dilepas (workspace sumber tetap di "
           ".local — hapus manual bila perlu)\n", name);
}

/* ============================================================
 *  eggkg list / search / info
 * ============================================================ */
static void egg_cmd_list(void) {
    db_load();
    if (g_ninst == 0) {
        printf("eggkg: belum ada paket terpasang\n");
    } else {
        printf("eggkg: paket terpasang (%d)\n", g_ninst);
        for (int i = 0; i < g_ninst; i++) {
            const char* v = g_inst[i].version;
            if (!v[0] || strcmp(v, "0") == 0) v = "-";
            printf("  %-14s %-8s %d file\n", g_inst[i].name, v,
                   g_inst[i].nfiles);
        }
    }
    if (egg_load_local_index() == 0 && g_npkg > 0)
        printf("index : %d paket tersedia (eggkg search)\n", g_npkg);
}

static void egg_cmd_search(const char* pat) {
    if (!pat || !pat[0]) {
        printf("eggkg: search <pola>\n");
        return;
    }
    if (egg_load_local_index() != 0) {
        printf("eggkg: index lokal belum ada — eggkg update dulu\n");
        return;
    }
    int n = 0;
    for (int i = 0; i < g_npkg; i++) {
        if (!egg_icontains(g_pkg[i].name, pat) &&
            !egg_icontains(g_pkg[i].desc, pat)) continue;
        db_load();
        struct egg_installed* inst = db_find(g_pkg[i].name);
        printf("  %-14s %-8s %-28s %s\n", g_pkg[i].name,
               g_pkg[i].version[0] ? g_pkg[i].version : "-",
               g_pkg[i].desc[0] ? g_pkg[i].desc : "-",
               inst ? "[terpasang]" : "");
        n++;
    }
    printf("eggkg: %d cocok untuk '%s'\n", n, pat);
}

static void egg_cmd_info(const char* name) {
    if (!name || !name[0]) {
        printf("eggkg: info <nama>\n");
        return;
    }
    if (egg_load_local_index() != 0) {
        printf("eggkg: index lokal belum ada — eggkg update dulu\n");
        return;
    }
    struct egg_entry* e = 0;
    for (int i = 0; i < g_npkg; i++)
        if (egg_ieq(g_pkg[i].name, name)) { e = &g_pkg[i]; break; }
    if (!e) {
        printf("eggkg: '%s' tidak ada di index\n", name);
        return;
    }

    printf("nama    : %s\n", e->name);
    printf("versi   : %s\n", e->version[0] ? e->version : "(v0 — "
           "package.list tanpa versi)");
    if (e->desc[0])  printf("deskripsi: %s\n", e->desc);
    if (e->deps[0])  printf("deps    : %s\n", e->deps);
    if (e->sha[0]) {
        char s16[17];
        egg_hex16(e->sha, s16);
        printf("sha256  : %s...\n", s16);
    }
    if (e->size)     printf("size    : %u bytes\n", (unsigned)e->size);
    if (e->url[0])   printf("arsip   : %s\n", e->url);
    for (int m = 0; m < e->nmirror; m++)
        printf("mirror%d : %s\n", m + 1, e->mirror[m]);
    if (e->nsrc) {
        printf("sumber  : %d file\n", e->nsrc);
        for (int i = 0; i < e->nsrc && i < 3; i++)
            printf("  - %s\n", e->src[i]);
        if (e->nsrc > 3) printf("  - ... (%d lagi)\n", e->nsrc - 3);
    }
    db_load();
    struct egg_installed* inst = db_find(e->name);
    if (inst) {
        printf("status  : TERPASANG (%s, %d file)\n",
               inst->version[0] ? inst->version : "-",
               inst->nfiles);
    } else {
        printf("status  : belum terpasang\n");
    }
}

/* ============================================================
 *  eggkg sync + auto-aktivasi boot (dependencies.bash)
 * ============================================================ */
static void egg_boot_sync(int verbose);

void eggkg_boot_check(void) {
    egg_boot_sync(0);
}

static void egg_boot_sync(int verbose) {
    /* flag [dependencies] bash di system.ecf aktif */
    int flag = 0;
    const char* act = ecf_active_path(0);
    if (act && act[0]) {
        const char* v = ecf_file_get(act, "dependencies.bash");
        if (v && egg_ieq(v, "true")) flag = 1;
    }

    char local[128];
    egg_local_dir(local, sizeof(local));
    struct fs_node* lroot = fs_get_node_from_path(fs_get_root(),
                                                  local);
    if (!lroot || !lroot->is_dir) return;

    struct fs_node* bindir = fs_get_node_from_path(fs_get_root(),
                                                   EGG_BIN_DIR);
    /* 0.4 Beta self-heal: /bin belum ada (belum pernah install) -> buat,
     * supaya sync tetap bisa mengisi (dipakai juga oleh alur
     * mtcc -make + eggkg sync tanpa install penuh). */
    if (!bindir && egg_mkdir_p(EGG_BIN_DIR) == 0) {
        bindir = fs_get_node_from_path(fs_get_root(), EGG_BIN_DIR);
    }
    int total = 0, copied = 0;

    /* total perintah = file .mrp di /bin (system path) */
    if (bindir) {
        fs_find_child(bindir, ".");       /* pemicu populate FAT    */
        for (struct fs_node* c = bindir->children; c; c = c->next)
            if (!c->is_dir && egg_ends_with(c->name, ".mrp")) total++;
    }

    /* self-heal: .local/<pkg>/*.mrp yang belum ada di /bin disalin
     * (satu-satu; move-style count tetap aman utk heap) */
    fs_find_child(lroot, ".");            /* pemicu populate FAT    */
    for (struct fs_node* pkg = lroot->children; pkg; pkg = pkg->next) {
        if (!pkg->is_dir) continue;
        fs_find_child(pkg, ".");          /* pemicu populate FAT    */
        for (struct fs_node* c = pkg->children; c; c = c->next) {
            if (c->is_dir || !egg_ends_with(c->name, ".mrp")) continue;
            if (!bindir) break;
            struct fs_node* b = fs_find_child(bindir, c->name);
            if (!b && fs_ensure_content(c) == 0 && c->content) {
                if (fs_write_binary(bindir, c->name,
                                    (const uint8_t*)c->content,
                                    c->size) == 0) {
                    copied++;
                    total++;
                }
            }
        }
    }

    if (!flag && total == 0) return;      /* boot bersih: diam      */

    if (copied) {
        printf("eggkg: sinkron %d perintah dari %s ke /bin\n",
               copied, local);
    }
    if (flag) {
        printf("eggkg: bash aktif — %d perintah standar tersedia "
               "(lf/showf = builtin)\n", total);
        if (total == 0)
            printf("eggkg: (eggkg install bash untuk mengisi)\n");
    } else if (verbose && total) {
        printf("eggkg: %d perintah terdeteksi di /bin\n", total);
    }
}

/* ============================================================
 *  Dispatch + help
 * ============================================================ */
static void egg_help(void) {
    printf("eggkg — package manager Equinox OS (server: GitHub)\n");
    printf("  update [sumber]      unduh package.list (+index.idx)\n");
    printf("                       sumber = URL atau path lokal\n");
    printf("  install <nama> [-y]  unduh sumber, build mtcc, pasang /bin\n");
    printf("  remove <nama> [-y]   hapus file tercatat di installed.db\n");
    printf("  list                 paket terpasang\n");
    printf("  search <pola>        cari di index\n");
    printf("  info <nama>          metadata paket\n");
    printf("  sync                 auto-detect .local -> /bin (juga\n");
    printf("                       jalan saat boot bila [dependencies]\n");
    printf("                       bash=true di system.ecf)\n");
    printf("\n");
    printf("  server default (ecf [eggkg] server=...):\n");
    printf("    %s\n", EGG_SERVER_DEFAULT);
}

void eggkg_cmd(const char* args) {
    while (*args == ' ') args++;

    char cmd[24];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 22) { cmd[i] = args[i]; i++; }
    cmd[i] = 0;
    const char* rest = args + i;
    while (*rest == ' ') rest++;

    if (!cmd[0] || egg_ieq(cmd, "help")) {
        egg_help();
        return;
    }
    if (egg_ieq(cmd, "update")) { egg_cmd_update(rest); return; }
    if (egg_ieq(cmd, "list"))   { egg_cmd_list();       return; }
    if (egg_ieq(cmd, "search")) { egg_cmd_search(rest); return; }
    if (egg_ieq(cmd, "info"))   { egg_cmd_info(rest);   return; }
    if (egg_ieq(cmd, "sync"))   { egg_boot_sync(1);     return; }

    if (egg_ieq(cmd, "install") || egg_ieq(cmd, "remove")) {
        /* parse: <nama> [-y] */
        char name[48];
        int  n = 0;
        int  yes = 0;
        while (*rest) {
            while (*rest == ' ') rest++;
            if (!*rest) break;
            char tok[24];
            int  tl = 0;
            while (*rest && *rest != ' ' && tl < 22)
                tok[tl++] = *rest++;
            tok[tl] = 0;
            if (strcmp(tok, "-y") == 0 || strcmp(tok, "--yes") == 0)
                yes = 1;
            else if (n == 0) {
                int k = 0;
                for (; tok[k] && n < 46; k++) name[n++] = tok[k];
                name[n] = 0;
            }
        }
        if (n == 0) {
            printf("eggkg: %s <nama> [-y]\n", cmd);
            return;
        }
        name[n] = 0;
        if (egg_ieq(cmd, "install")) egg_cmd_install(name, yes);
        else                         egg_cmd_remove(name, yes);
        return;
    }

    printf("eggkg: perintah '%s' tidak dikenal — eggkg help\n", cmd);
}
