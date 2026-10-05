/* ============================================================
 *  ecf.c — Equinox Config File (.ecf) parser + aksesor
 * ------------------------------------------------------------
 *  Lihat header/library/header/ecf.h untuk format & batasnya.
 *
 *  Semua buffer BESAR bersifat file-scope static (bukan stack):
 *    - g_store        : store global, 64 x (64 + 128 + 1)  ~12,5 KB
 *    - ecf_work       : buffer kerja patch/tulis           8 KB + 2
 *    - ecf_linebuf    : satu baris parse/patch             161 B
 *    - ecf_pathbuf    : hasil ecf_active_path()            64 B
 *
 *  kernel/*.c dikompilasi dengan i686-elf-g++ (arti: C++) — tanpa
 *  designated initializer, tapi header tetap pakai guard extern "C".
 * ============================================================ */
#include "library/header/ecf.h"
#include "library/header/fs_ram.h"
#include "library/header/libstring.h"

/* ----------------------------------------------------------------
 *  Skema fase 1 — hanya net.driver (ne2000 | e1000 | none).
 *  Dipakai ecf_check / ecf_key_values / ecf_needs_reboot dan
 *  pesan error dari builtin `set`.
 *
 *  0.4 Beta: dua kunci BEBAS (values == NULL = nilai apa pun boleh):
 *    base.path     — pivot base yang dibuat `set -b` (diterapkan
 *                    di akhir fat32_boot_init(), bukan runtime)
 *    active.conf   — pointer: system.ecf menunjuk berkas .ecf
 *                    utama yang di-overlay saat boot
 * ---------------------------------------------------------------- */
#define ECF_SCHEMA_N (sizeof(ecf_schema) / sizeof(ecf_schema[0]))

static const struct {
    const char* key;
    const char* values;     /* NULL = nilai bebas (free-form) */
    int         reboot;
} ecf_schema[] = {
    { "net.driver", "ne2000|e1000|none", 1 },
    { "base.path",  NULL,                0 },
    { "active.conf", NULL,               0 },
    /* 0.4 Beta — eggkg package manager + header [dependencies]
     * (permintaan user). dependencies.bash=true = auto-detect
     * /equinox/.local saat shell siap (eggkg_boot_check). */
    { "dependencies.bash", "true|false", 0 },
    { "eggkg.server",      NULL,         0 },
    { "eggkg.mirror",      NULL,         0 },
    { "eggkg.local",       NULL,         0 },
};

/* Nilai placeholder buatan `set -d ... -base ...`: key-nya valid,
 * value-nya sengaja dikosongkan supaya user mengisi sendiri. */
#define ECF_NONE_VAL "NONE"

/* ----------------------------------------------------------------
 *  Buffer statis
 * ---------------------------------------------------------------- */
static struct ecf_store g_store;
static int              g_store_loaded;

static char ecf_work[ECF_FILE_MAX + 2];
static char ecf_linebuf[ECF_LINE_MAX + 2];
static char ecf_pathbuf[ECF_PATH_MAX];

/* 0.4 Beta — target override (`set -d`). NULL berarti pakai perilaku lama. */
static char ecf_tgtbuf[ECF_PATH_MAX];
static int  ecf_tgt_on;

/* ----------------------------------------------------------------
 *  Utilitas string kecil (libstring punya semuanya, tanpa precision
 *  di snprintf — jadi semua salinan dilakukan manual/bounded)
 * ---------------------------------------------------------------- */
static int ecf_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/* Potong spasi di kedua ujung (in-place, NUL-terminated). */
static void ecf_trim(char* s) {
    uint32_t n = (uint32_t)strlen(s);
    while (n > 0 && ecf_is_space(s[n - 1])) { s[n - 1] = '\0'; n--; }
    uint32_t k = 0;
    while (s[k] && ecf_is_space(s[k])) k++;
    if (k > 0) {
        uint32_t i = 0;
        while (s[i + k]) { s[i] = s[i + k]; i++; }
        s[i] = '\0';
    }
}

/* Salin terbatas + selalu NUL-terminate. */
static void ecf_copy(char* dst, uint32_t dstsz, const char* src) {
    uint32_t i = 0;
    if (dstsz == 0) return;
    while (src && src[i] && i < dstsz - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int ecf_same(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* 0.4 Beta: apakah `s` diawali `pre`? */
static int ecf_prefix(const char* s, const char* pre) {
    if (!s || !pre) return 0;
    while (*pre) { if (*s != *pre) return 0; s++; pre++; }
    return 1;
}

static int ecf_schema_index(const char* key) {
    for (uint32_t i = 0; i < ECF_SCHEMA_N; i++)
        if (ecf_same(key, ecf_schema[i].key)) return (int)i;
    return -1;
}

/* "a ada di pipe-list b?" — cocok persis, dipisah '|'. */
static int ecf_value_ok(const char* list, const char* val) {
    const char* p = list;
    while (*p) {
        const char* s = p;
        while (*p && *p != '|') p++;
        uint32_t n = (uint32_t)(p - s);
        uint32_t m = (uint32_t)strlen(val);
        if (n == m) {
            uint32_t k = 0;
            while (k < n && s[k] == val[k]) k++;
            if (k == n) return 1;
        }
        if (*p == '|') p++;
    }
    return 0;
}

/* ----------------------------------------------------------------
 *  API validasi
 * ---------------------------------------------------------------- */
int ecf_check(const char* key, const char* val) {
    if (!key || !key[0]) return 1;
    int i = ecf_schema_index(key);
    if (i < 0) return 1;
    if (!val) return 0;
    /* 0.4 Beta: kunci free-form (values == NULL) menerima nilai apa pun
     * asal tidak kosong. */
    if (!ecf_schema[i].values) return val[0] ? 0 : 2;
    return ecf_value_ok(ecf_schema[i].values, val) ? 0 : 2;
}

const char* ecf_key_values(const char* key) {
    if (!key) return NULL;
    int i = ecf_schema_index(key);
    return (i < 0) ? NULL : ecf_schema[i].values;
}

int ecf_needs_reboot(const char* key) {
    if (!key) return 0;
    int i = ecf_schema_index(key);
    return (i < 0) ? 0 : ecf_schema[i].reboot;
}

/* ----------------------------------------------------------------
 *  Store: reset / get / put
 * ---------------------------------------------------------------- */
static void ecf_store_reset(struct ecf_store* st) {
    if (!st) return;
    for (int i = 0; i < ECF_MAX_ENTRIES; i++) {
        st->ent[i].key[0] = '\0';
        st->ent[i].val[0] = '\0';
        st->ent[i].used = 0;
    }
    st->count = 0;
    st->warn = 0;
    st->err = 0;
    st->none = 0;
}

const char* ecf_get(const struct ecf_store* st, const char* key) {
    if (!st || !key) return NULL;
    for (int i = 0; i < ECF_MAX_ENTRIES; i++) {
        if (st->ent[i].used && ecf_same(st->ent[i].key, key))
            return st->ent[i].val;
    }
    return NULL;
}

int ecf_put(struct ecf_store* st, const char* key, const char* val) {
    if (!st || !key || !val) return -1;
    if (strlen(key) >= ECF_KEY_MAX) return -1;
    if (strlen(val) >= ECF_VAL_MAX) return -1;

    int slot = -1;
    for (int i = 0; i < ECF_MAX_ENTRIES; i++) {
        if (!st->ent[i].used) { if (slot < 0) slot = i; continue; }
        if (ecf_same(st->ent[i].key, key)) { slot = i; break; }  /* last-wins */
    }
    if (slot < 0) return -1;                       /* penuh */

    if (!st->ent[slot].used) st->count++;
    st->ent[slot].used = 1;
    ecf_copy(st->ent[slot].key, ECF_KEY_MAX, key);
    ecf_copy(st->ent[slot].val, ECF_VAL_MAX, val);
    return 0;
}

/* ----------------------------------------------------------------
 *  Parser
 * ---------------------------------------------------------------- */
static int ecf_parse_into(const char* buf, uint32_t len,
                          struct ecf_store* st, int do_reset) {
    if (!st) return -1;
    if (len > ECF_FILE_MAX) return -1;
    if (do_reset) ecf_store_reset(st);
    if (!buf || len == 0) return 0;

    char section[ECF_KEY_MAX];
    section[0] = '\0';

    uint32_t i = 0;
    while (i < len) {
        /* ---- satu baris ---- */
        uint32_t ls = i;
        while (i < len && buf[i] != '\n') i++;
        uint32_t ll = i - ls;
        if (i < len) i++;                       /* consume '\n' */
        if (ll > ECF_LINE_MAX) { st->err++; continue; }
        if (ll == 0) continue;

        char* line = ecf_linebuf;
        for (uint32_t k = 0; k < ll; k++) line[k] = buf[ls + k];
        line[ll] = '\0';

        /* leading whitespace (trailing dipotong ecf_trim) */
        uint32_t off = 0;
        while (line[off] && ecf_is_space(line[off])) off++;
        if (off) {
            uint32_t k = 0;
            while (line[k + off]) { line[k] = line[k + off]; k++; }
            line[k] = '\0';
        }
        ecf_trim(line);
        if (!line[0]) continue;                 /* baris kosong */
        if (line[0] == '#') continue;           /* komentar baris penuh */

        /* ---- [section] ---- */
        if (line[0] == '[') {
            uint32_t n = (uint32_t)strlen(line);
            if (n < 3 || line[n - 1] != ']') { st->err++; continue; }
            line[n - 1] = '\0';
            ecf_trim(line + 1);
            if (!line[1]) { st->err++; continue; }
            if (strlen(line + 1) >= ECF_KEY_MAX) { st->err++; continue; }
            ecf_copy(section, ECF_KEY_MAX, line + 1);
            continue;
        }

        /* ---- key = value ---- */
        char* eq = strchr(line, '=');
        if (!eq) { st->err++; continue; }       /* baris rusak */
        *eq = '\0';
        char* key = line;
        char* val = eq + 1;
        ecf_trim(key);
        ecf_trim(val);
        if (!key[0]) { st->err++; continue; }

        char full[ECF_KEY_MAX];
        if (strchr(key, '.') || !section[0]) {
            ecf_copy(full, ECF_KEY_MAX, key);   /* key berisi '.' -> apa adanya */
        } else if (strlen(section) + 1 + strlen(key) >= ECF_KEY_MAX) {
            st->err++;
            continue;
        } else {
            snprintf(full, sizeof(full), "%s.%s", section, key);
        }
        if (!full[0] || strlen(full) >= ECF_KEY_MAX) { st->err++; continue; }

        int c = ecf_check(full, NULL);          /* 0.4 Beta: cek key saja */
        if (c != 0) { st->warn++; continue; }   /* key tak dikenal    */

        /* 0.4 Beta: placeholder "NONE" = key valid yang belum diisi user
         * (`set -d ... -base ...`). Dilewati, bukan error — init yang
         * membacanya akan jatuh ke default bila semua key masih NONE. */
        if (ecf_same(val, ECF_NONE_VAL)) { st->none++; continue; }

        if (ecf_check(full, val) != 0) { st->err++; continue; }  /* nilai invalid */

        if (ecf_put(st, full, val) != 0) st->err++;
    }
    return 0;
}

int ecf_parse(const char* buf, uint32_t len, struct ecf_store* st) {
    return ecf_parse_into(buf, len, st, 1);
}

/* 0.4 Beta: parse KE store yang sudah terisi (tanpa reset) — dipakai
 * overlay `active.conf` saat boot. */
int ecf_parse_merge(const char* buf, uint32_t len, struct ecf_store* st) {
    return ecf_parse_into(buf, len, st, 0);
}

/* ----------------------------------------------------------------
 *  ecf_load — baca berkas dari path absolut lalu parse
 * ---------------------------------------------------------------- */
int ecf_load(const char* path, struct ecf_store* st) {
    if (!path || !st) return -1;
    struct fs_node* n = fs_get_node_from_path(fs_get_root(), path);
    if (!n || n->is_dir) return -1;
    if (fs_ensure_content(n) != 0) return -1;
    if (n->size > ECF_FILE_MAX) return -1;
    if (n->size > 0 && !n->content) return -1;
    return ecf_parse(n->content ? n->content : "", n->size, st);
}

/* 0.4 Beta: sama dengan ecf_load tetapi MENAMBAH entri (last-wins). */
int ecf_load_merge(const char* path, struct ecf_store* st) {
    if (!path || !st) return -1;
    struct fs_node* n = fs_get_node_from_path(fs_get_root(), path);
    if (!n || n->is_dir) return -1;
    if (fs_ensure_content(n) != 0) return -1;
    if (n->size > ECF_FILE_MAX) return -1;
    if (n->size > 0 && !n->content) return -1;
    return ecf_parse_merge(n->content ? n->content : "", n->size, st);
}

/* ----------------------------------------------------------------
 *  ecf_active_path — 0.4 Beta: kanonik /equinox/conf/system.ecf
 *    baca : /equinox/conf/system.ecf -> /mnt/equinox/conf/system.ecf
 *           -> /boot/system.ecf (legacy) -> /mnt/boot/system.ecf
 *           (legacy) -> NULL
 *    tulis: pastikan /equinox/conf ada (volume /mnt diprioritaskan,
 *           lalu RAMFS) -> fallback dir boot legacy yang sudah ada ->
 *           fs_create_dir(root,"boot") + /boot/system.ecf
 *
 *  CATATAN perubahan urutan dir: RAMFS root MEMANG punya dir
 *  /equinox (modul ISO: tools/repo/.local), jadi penulisan selalu
 *  mencoba volume /mnt dulu supaya konfigurasi benar-benar persisten.
 *  Instal lama (system.ecf masih di boot/) tetap terbaca lewat
 *  fallback, dan `set` pertama otomatis membuat conf/ di volume —
 *  migrasi lokasi berjalan tanpa langkah manual.
 * ---------------------------------------------------------------- */
static struct fs_node* ecf_ensure_subdir(struct fs_node* parent,
                                         const char* name) {
    if (!parent || !parent->is_dir) return NULL;
    struct fs_node* d = fs_find_child(parent, name);
    if (d && d->is_dir) return d;
    if (fs_create_dir(parent, name) != 0) return NULL;
    d = fs_find_child(parent, name);
    return (d && d->is_dir) ? d : NULL;
}

const char* ecf_active_path(int for_write) {
    struct fs_node* root = fs_get_root();
    if (!root) return NULL;

    /* 0.4 Beta: `set -d FILE` menunjuk berkas lain — SEMUA akses
     * (baca maupun tulis) diarahkan ke sana selama sesi ini. */
    if (ecf_tgt_on) return ecf_tgtbuf;

    /* 0.4 Beta: conf/ lebih dulu (kanonik), legacy boot/ menyusul. */
    static const char* const FILES[4] = {
        "/equinox/conf/system.ecf", "/mnt/equinox/conf/system.ecf",
        "/boot/system.ecf", "/mnt/boot/system.ecf"
    };
    for (int i = 0; i < 4; i++) {
        struct fs_node* n = fs_get_node_from_path(root, FILES[i]);
        if (n && !n->is_dir) {
            ecf_copy(ecf_pathbuf, ECF_PATH_MAX, FILES[i]);
            return ecf_pathbuf;
        }
    }
    if (!for_write) return NULL;

    /* 0.4 Beta: tulis ke /equinox/conf — volume (/mnt) menang atas
     * RAMFS live (sesi-only). Dir dibuat bila belum ada, jadi
     * instal lama bermigrasi otomatis pada `set` pertama. */
    {
        struct fs_node* cand[2];
        cand[0] = fs_get_node_from_path(root, "/mnt");
        cand[1] = root;
        for (int i = 0; i < 2; i++) {
            struct fs_node* eq   = ecf_ensure_subdir(cand[i], "equinox");
            struct fs_node* conf = ecf_ensure_subdir(eq, "conf");
            if (conf) {
                ecf_copy(ecf_pathbuf, ECF_PATH_MAX,
                         (i == 0) ? "/mnt/equinox/conf" : "/equinox/conf");
                uint32_t l = (uint32_t)strlen(ecf_pathbuf);
                const char* suf = "/system.ecf";
                for (uint32_t k = 0; suf[k] && l + 1 < ECF_PATH_MAX; k++)
                    ecf_pathbuf[l++] = suf[k];
                ecf_pathbuf[l] = '\0';
                return ecf_pathbuf;
            }
        }
    }

    /* Fallback legacy: dir boot lama yang sudah ada. */
    static const char* const DIRS[2] = { "/mnt/boot", "/boot" };
    for (int i = 0; i < 2; i++) {
        struct fs_node* n = fs_get_node_from_path(root, DIRS[i]);
        if (n && n->is_dir) {
            ecf_copy(ecf_pathbuf, ECF_PATH_MAX, DIRS[i]);
            uint32_t l = (uint32_t)strlen(ecf_pathbuf);
            const char* suf = "/system.ecf";
            for (uint32_t k = 0; suf[k] && l + 1 < ECF_PATH_MAX; k++)
                ecf_pathbuf[l++] = suf[k];
            ecf_pathbuf[l] = '\0';
            return ecf_pathbuf;
        }
    }

    /* RAMFS live tanpa volume: buat /boot dulu (session-only). */
    if (fs_create_dir(root, "boot") != 0) {
        struct fs_node* d = fs_find_child(root, "boot");
        if (!d || !d->is_dir) return NULL;
    }
    ecf_copy(ecf_pathbuf, ECF_PATH_MAX, "/boot/system.ecf");
    return ecf_pathbuf;
}

/* ----------------------------------------------------------------
 *  Pecah path absolut -> direktori induk + nama berkas.
 *  Mengisi dir[] dan mengembalikan pointer nama di dalam `path`.
 * ---------------------------------------------------------------- */
static const char* ecf_split(const char* path, char* dir, uint32_t dirsz) {
    if (!path || path[0] != '/' || !dir || dirsz < 2) return NULL;
    uint32_t plen = (uint32_t)strlen(path);
    if (plen < 2 || plen >= dirsz) return NULL;

    uint32_t last = 0;
    for (uint32_t i = 0; i < plen; i++) if (path[i] == '/') last = i;
    if (last == 0) {
        dir[0] = '/';
        dir[1] = '\0';
    } else {
        if (last >= dirsz) return NULL;
        for (uint32_t i = 0; i < last; i++) dir[i] = path[i];
        dir[last] = '\0';
    }
    return path + last + 1;
}

/* ----------------------------------------------------------------
 *  0.4 Beta — kandidat path berkas OVERLAY dari pointer `active.conf`
 *
 *  ac absolut  -> apa adanya, lalu remap /mnt (volume bisa saja
 *                 bergeser antara saat `set -d` dan saat boot)
 *  ac relatif  -> <direktori berkas p> + "/" + ac
 *  Mengembalikan jumlah kandidat (0..3).
 * ---------------------------------------------------------------- */
static int ecf_active_cands(const char* p, const char* ac,
                            char cand[3][ECF_PATH_MAX]) {
    int nc = 0;
    if (!p || !ac || !ac[0]) return 0;

    if (ac[0] == '/') {
        ecf_copy(cand[nc++], ECF_PATH_MAX, ac);
        if (ecf_prefix(ac, "/mnt/")) {
            ecf_copy(cand[nc++], ECF_PATH_MAX, ac + 4);     /* -> / ...   */
        } else if (!ecf_same(ac, "/mnt")) {
            uint32_t l = 0;
            const char* pre = "/mnt";
            for (; pre[l] && l < ECF_PATH_MAX - 1; l++) cand[nc][l] = pre[l];
            for (uint32_t k = 0; ac[k] && l + 1 < ECF_PATH_MAX; k++)
                cand[nc][l++] = ac[k];
            cand[nc][l] = '\0';
            nc++;
        }
    } else {
        char dir[ECF_PATH_MAX];
        if (ecf_split(p, dir, sizeof(dir))) {
            uint32_t l = (uint32_t)strlen(dir);
            if (l + 1 >= ECF_PATH_MAX) return 0;
            for (uint32_t i = 0; i < l; i++) cand[nc][i] = dir[i];
            if (l && dir[l - 1] != '/') cand[nc][l++] = '/';
            for (uint32_t k = 0; ac[k] && l + 1 < ECF_PATH_MAX; k++)
                cand[nc][l++] = ac[k];
            cand[nc][l] = '\0';
            nc++;
        }
    }
    return nc;
}

/* ----------------------------------------------------------------
 *  0.4 Beta — pemindai SATU key (tanpa store, tanpa validasi skema)
 *  1 = ketemu; `out` berisi nilai terakhir (last-wins).
 * ---------------------------------------------------------------- */
static int ecf_scan_one(const char* buf, uint32_t len, const char* want,
                        char* out, uint32_t outsz) {
    static char full[ECF_KEY_MAX];
    char sec[ECF_KEY_MAX];
    if (!buf || !want || !out || outsz == 0) return 0;
    out[0] = '\0';
    sec[0] = '\0';

    uint32_t i = 0;
    while (i < len) {
        uint32_t s = i;
        while (i < len && buf[i] != '\n') i++;
        uint32_t l = i - s;
        if (i < len) i++;
        if (l > ECF_LINE_MAX) l = ECF_LINE_MAX;
        for (uint32_t k = 0; k < l; k++) ecf_linebuf[k] = buf[s + k];
        ecf_linebuf[l] = '\0';
        ecf_trim(ecf_linebuf);
        if (!ecf_linebuf[0] || ecf_linebuf[0] == '#') continue;

        if (ecf_linebuf[0] == '[') {
            char* br = ecf_linebuf;
            uint32_t k = 1;
            while (br[k] && br[k] != ']') k++;
            if (br[k] == ']') { br[k] = '\0'; ecf_copy(sec, sizeof(sec), br + 1); }
            continue;
        }

        uint32_t eqp = 0;
        while (ecf_linebuf[eqp] && ecf_linebuf[eqp] != '=') eqp++;
        if (!ecf_linebuf[eqp]) continue;
        ecf_linebuf[eqp] = '\0';
        char* val = ecf_linebuf + eqp + 1;
        ecf_trim(ecf_linebuf);
        ecf_trim(val);
        if (!ecf_linebuf[0]) continue;

        int dotted = 0;
        for (uint32_t k = 0; ecf_linebuf[k]; k++)
            if (ecf_linebuf[k] == '.') dotted = 1;
        if (dotted || !sec[0]) {
            ecf_copy(full, sizeof(full), ecf_linebuf);
        } else {
            uint32_t n = 0;
            for (; sec[n] && n < sizeof(full) - 1; n++) full[n] = sec[n];
            if (n < sizeof(full) - 1) full[n++] = '.';
            for (uint32_t k = 0; ecf_linebuf[k] && n < sizeof(full) - 1; k++)
                full[n++] = ecf_linebuf[k];
            full[n] = '\0';
        }
        if (ecf_same(full, want)) ecf_copy(out, outsz, val);
    }
    return out[0] != '\0';
}

/* Ambil isi berkas ke ecf_work; kembali dengan panjang, <0 = gagal. */
static int ecf_read_file(const char* path, uint32_t* len_out) {
    struct fs_node* n = fs_get_node_from_path(fs_get_root(), path);
    if (!n || n->is_dir) { *len_out = 0; return 0; }   /* belum ada */
    if (fs_ensure_content(n) != 0) return -1;
    if (n->size > ECF_FILE_MAX) return -1;
    if (n->size > 0 && !n->content) return -1;
    uint32_t len = n->size;
    for (uint32_t i = 0; i < len; i++) ecf_work[i] = n->content[i];
    ecf_work[len] = '\0';
    *len_out = len;
    return 0;
}

/* ----------------------------------------------------------------
 *  0.4 Beta — ecf_file_get: baca SATU key dari sebuah berkas .ecf
 *  TANPA menyentuh store global (dipakai fat32_boot_init untuk
 *  `base.path`). Mengikuti pointer `active.conf` bila key tidak ada
 *  di berkas utama. Mengembalikan buffer statis — valid sampai
 *  panggilan berikutnya — atau NULL bila tidak ketemu.
 * ---------------------------------------------------------------- */
const char* ecf_file_get(const char* path, const char* key) {
    static char res[ECF_VAL_MAX];
    static char ac[ECF_PATH_MAX];
    res[0] = '\0';
    ac[0]  = '\0';
    if (!path || !key || !key[0]) return NULL;

    uint32_t len = 0;
    if (ecf_read_file(path, &len) != 0) return NULL;
    if (len) {
        ecf_scan_one(ecf_work, len, key, res, sizeof(res));
        if (!res[0])
            ecf_scan_one(ecf_work, len, ECF_ACTIVE_KEY, ac, sizeof(ac));
    }
    if (!res[0] && ac[0]) {
        char cand[3][ECF_PATH_MAX];
        int nc = ecf_active_cands(path, ac, cand);
        for (int i = 0; i < nc; i++) {
            struct fs_node* n = fs_get_node_from_path(fs_get_root(), cand[i]);
            if (!n || n->is_dir) continue;
            uint32_t l2 = 0;
            if (ecf_read_file(cand[i], &l2) != 0 || !l2) continue;
            ecf_scan_one(ecf_work, l2, key, res, sizeof(res));
            if (res[0]) break;
        }
    }
    return res[0] ? res : NULL;
}

static int ecf_write_file(const char* dir, const char* fname,
                          uint32_t len) {
    struct fs_node* parent = fs_get_node_from_path(fs_get_root(), dir);
    if (!parent || !parent->is_dir) return -1;
    if (!len) { ecf_work[0] = '#'; ecf_work[1] = '\n'; len = 2; }
    ecf_work[len] = '\0';
    return fs_write_binary(parent, fname, (const uint8_t*)ecf_work, len);
}

/* ----------------------------------------------------------------
 *  ecf_set_file — patch IN-PLACE
 *
 *  urutan memmove: delta > 0 -> memmove ekor dulu, baru tulis value;
 *                  delta < 0 -> tulis value dulu, baru memmove ekor.
 * ---------------------------------------------------------------- */
int ecf_set_file(const char* path, const char* key, const char* val) {
    if (!path || !key || !val) return -1;
    if (strlen(key) >= ECF_KEY_MAX || strlen(val) >= ECF_VAL_MAX)
        return -1;

    char dir[256];
    const char* fname = ecf_split(path, dir, sizeof(dir));
    if (!fname || !fname[0]) return -1;

    uint32_t len = 0;
    if (ecf_read_file(path, &len) != 0) return -1;

    /* ---- cari baris terAKHIR dengan full-key yang sama ---- */
    int     have = 0;
    uint32_t vabs = 0, vend = 0;
    char section[ECF_KEY_MAX];
    section[0] = '\0';

    uint32_t i = 0;
    while (i < len) {
        uint32_t ls = i;
        while (i < len && ecf_work[i] != '\n') i++;
        uint32_t ll = i - ls;
        if (i < len) i++;
        if (ll == 0 || ll > ECF_LINE_MAX) continue;

        uint32_t p = ls;
        while (p < ls + ll && ecf_is_space(ecf_work[p])) p++;
        if (p >= ls + ll) continue;
        if (ecf_work[p] == '#') continue;

        if (ecf_work[p] == '[') {
            uint32_t e = p;
            while (e < ls + ll && ecf_work[e] != ']') e++;
            if (e >= ls + ll || e == p + 1) { section[0] = '\0'; continue; }
            uint32_t n = 0;
            uint32_t q = p + 1;
            while (q < e && n < ECF_KEY_MAX - 1) {
                if (!ecf_is_space(ecf_work[q])) section[n++] = ecf_work[q];
                q++;
            }
            section[n] = '\0';
            continue;
        }

        uint32_t e = p;
        while (e < ls + ll && ecf_work[e] != '=') e++;
        if (e >= ls + ll) continue;

        /* full key = [p, e) yang sudah di-trim kanan + section */
        uint32_t ke = e;
        while (ke > p && ecf_is_space(ecf_work[ke - 1])) ke--;
        uint32_t klen = ke - p;
        if (klen == 0 || klen >= ECF_KEY_MAX) continue;

        char full[ECF_KEY_MAX];
        int has_dot = 0;
        for (uint32_t k = p; k < ke; k++)
            if (ecf_work[k] == '.') { has_dot = 1; break; }

        if (has_dot || !section[0]) {
            for (uint32_t k = 0; k < klen; k++) full[k] = ecf_work[p + k];
            full[klen] = '\0';
        } else {
            /* section.key — salin manual (snprintf tanpa precision) */
            uint32_t n = 0;
            for (uint32_t k = 0; section[k] && n < ECF_KEY_MAX - 1; k++)
                full[n++] = section[k];
            if (n < ECF_KEY_MAX - 1) full[n++] = '.';
            for (uint32_t k = 0; k < klen && n < ECF_KEY_MAX - 1; k++)
                full[n++] = ecf_work[p + k];
            full[n] = '\0';
        }

        if (!ecf_same(full, key)) continue;

        /* value: setelah '=' (lewati spasi) sampai akhir baris
         * (buang spasi trailing) */
        uint32_t vs = e + 1;
        while (vs < ls + ll && ecf_is_space(ecf_work[vs])) vs++;
        uint32_t ve = ls + ll;
        while (ve > vs && ecf_is_space(ecf_work[ve - 1])) ve--;
        have = 1;
        vabs = vs;
        vend = ve;
    }

    uint32_t newlen = (uint32_t)strlen(val);

    if (have) {
        int delta = (int)newlen - (int)(vend - vabs);
        if (delta > 0) {
            if (len + (uint32_t)delta > ECF_FILE_MAX) return -1;
            /* ekor dulu, baru value */
            memmove(ecf_work + vend + delta, ecf_work + vend,
                    len - vend);
            for (uint32_t k = 0; k < newlen; k++)
                ecf_work[vabs + k] = val[k];
            len = (uint32_t)((int)len + delta);
        } else if (delta < 0) {
            /* value dulu, baru memmove ekor */
            for (uint32_t k = 0; k < newlen; k++)
                ecf_work[vabs + k] = val[k];
            memmove(ecf_work + vabs + newlen, ecf_work + vend,
                    len - vend);
            len = (uint32_t)((int)len + delta);
        } else {
            for (uint32_t k = 0; k < newlen; k++)
                ecf_work[vabs + k] = val[k];
        }
        ecf_work[len] = '\0';
        return ecf_write_file(dir, fname, len);
    }

    /* ---- tak ada baris: append "full.key = value\n" ---- */
    char add[ECF_KEY_MAX + ECF_VAL_MAX + 4];
    uint32_t alen = 0;
    /* salin manual (snprintf tanpa precision, tapi ini %-s biasa) */
    {
        uint32_t n = 0;
        for (const char* s = key; *s && n < sizeof(add) - 2; s++)
            add[n++] = *s;
        if (n < sizeof(add) - 2) add[n++] = ' ';
        if (n < sizeof(add) - 2) add[n++] = '=';
        if (n < sizeof(add) - 2) add[n++] = ' ';
        for (const char* s = val; *s && n < sizeof(add) - 2; s++)
            add[n++] = *s;
        if (n < sizeof(add) - 1) add[n++] = '\n';
        add[n] = '\0';
        alen = n;
    }
    if (len > 0 && ecf_work[len - 1] != '\n') {
        if (len + 1 + alen > ECF_FILE_MAX) return -1;
        ecf_work[len++] = '\n';
    } else if (len + alen > ECF_FILE_MAX) {
        return -1;
    }
    for (uint32_t k = 0; k < alen; k++) ecf_work[len + k] = add[k];
    len += alen;
    ecf_work[len] = '\0';
    return ecf_write_file(dir, fname, len);
}

/* ----------------------------------------------------------------
 *  ecf_write_store — serialisasi seluruh store (key = value\n)
 * ---------------------------------------------------------------- */
int ecf_write_store(const char* path, struct ecf_store const* st) {
    if (!path || !st) return -1;

    char dir[256];
    const char* fname = ecf_split(path, dir, sizeof(dir));
    if (!fname || !fname[0]) return -1;

    uint32_t len = 0;
    for (int i = 0; i < ECF_MAX_ENTRIES; i++) {
        if (!st->ent[i].used) continue;
        char line[ECF_KEY_MAX + ECF_VAL_MAX + 8];
        uint32_t n = 0;
        for (const char* s = st->ent[i].key; *s && n < sizeof(line) - 2; s++)
            line[n++] = *s;
        if (n < sizeof(line) - 2) line[n++] = ' ';
        if (n < sizeof(line) - 2) line[n++] = '=';
        if (n < sizeof(line) - 2) line[n++] = ' ';
        for (const char* s = st->ent[i].val; *s && n < sizeof(line) - 2; s++)
            line[n++] = *s;
        if (n < sizeof(line) - 1) line[n++] = '\n';
        line[n] = '\0';

        if (len + n > ECF_FILE_MAX) return -1;
        for (uint32_t k = 0; k < n; k++) ecf_work[len + k] = line[k];
        len += n;
    }
    ecf_work[len] = '\0';
    return ecf_write_file(dir, fname, len);
}

/* ----------------------------------------------------------------
 *  ecf_store — store global, lazy-load sekali
 *  0.4 Beta: setelah system.ecf, pointer `active.conf` menunjuk berkas
 *  konf UTAMA yang di-overlay di atasnya (last-wins) — jadi
 *  `set -d drivers.ecf` benar-benar ikut dibaca saat boot.
 * ---------------------------------------------------------------- */
struct ecf_store* ecf_store(void) {
    if (!g_store_loaded) {
        g_store_loaded = 1;
        ecf_store_reset(&g_store);
        const char* p = ecf_active_path(0);
        if (p) ecf_load(p, &g_store);

        const char* ac = ecf_tgt_on ? NULL : ecf_get(&g_store, ECF_ACTIVE_KEY);
        if (p && ac && ac[0]) {
            char cand[3][ECF_PATH_MAX];
            int nc = ecf_active_cands(p, ac, cand);
            for (int i = 0; i < nc; i++) {
                if (ecf_same(cand[i], p)) continue;
                struct fs_node* n = fs_get_node_from_path(fs_get_root(),
                                                          cand[i]);
                if (n && !n->is_dir) { ecf_load_merge(cand[i], &g_store); break; }
            }
        }
    }
    return &g_store;
}

/* ============================================================
 *  0.4 Beta — target override (`set -d FILE [-path DIR]`)
 * ============================================================ */
const char* ecf_target(void) {
    return ecf_tgt_on ? ecf_tgtbuf : NULL;
}

void ecf_store_invalidate(void) {
    g_store_loaded = 0;
}

/* Buat direktori absolut secara rekursif (komponen demi komponen). */
#define ECF_DIRNAME_MAX 64      /* == FS_NAME_MAX (fs_ram.cpp local) */
static int ecf_mkdir_p(const char* dir) {
    struct fs_node* root = fs_get_root();
    if (!root || !dir || dir[0] != '/') return -1;
    struct fs_node* cur = root;
    uint32_t i = 0;
    while (dir[i]) {
        while (dir[i] == '/') i++;
        if (!dir[i]) break;
        uint32_t s = i;
        while (dir[i] && dir[i] != '/') i++;
        uint32_t n = i - s;
        if (n == 0 || n >= ECF_DIRNAME_MAX) return -1;
        char name[ECF_DIRNAME_MAX];
        for (uint32_t k = 0; k < n; k++) name[k] = dir[s + k];
        name[n] = '\0';
        struct fs_node* nx = fs_find_child(cur, name);
        if (!nx) {
            if (fs_create_dir(cur, name) != 0) return -1;
            nx = fs_find_child(cur, name);
        }
        if (!nx || !nx->is_dir) return -1;
        cur = nx;
    }
    return 0;
}

int ecf_target_set(const char* path) {
    if (!path || path[0] != '/') return -1;
    if (strlen(path) >= ECF_PATH_MAX) return -1;

    /* direktori induk wajib ada — ecf_write_file menolak parent hilang */
    char dir[ECF_PATH_MAX];
    if (!ecf_split(path, dir, sizeof(dir))) return -1;
    if (ecf_mkdir_p(dir) != 0) return -1;

    ecf_copy(ecf_tgtbuf, ECF_PATH_MAX, path);
    ecf_tgt_on = 1;
    ecf_store_invalidate();
    return 0;
}

void ecf_target_clear(void) {
    ecf_tgt_on = 0;
    ecf_tgtbuf[0] = '\0';
    ecf_store_invalidate();
}
