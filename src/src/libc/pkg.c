/* libc/pkg.c — package builder API (v0.9.3).
   Membikin package jadi gampang: mkdir -p, copy file/dir rekursif,
   move, remove rekursif, tulis/baca teks, list direktori, manifest,
   dan pasang artefak ke /bin. Dibangun di atas syscalls #40-#48
   (mtcc builtins __sys_*) — tanpa struct, mengikuti dialek mtcc.
   Spliced via <morph.h>; guard aman untuk double-include. */
#ifndef LIBC_PKG_C
#define LIBC_PKG_C
#include "/equinox/libc/memory.c"                /* dependency */
#include "/equinox/libc/string.c"                /* dependency */
#include "/equinox/libc/heap.c"                  /* dependency (free) */

#define PKG_O_DIR     4096        /* SYS_O_DIR (lihat <syscall.h>)  */
#define PKG_BUF_CAP   (96 * 1024) /* batas salin per berkas         */

/* ---- inspeksi ------------------------------------------------ */
int pkg_is_dir(char* path) {
    int fd = __sys_open2(path, PKG_O_DIR);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}
int pkg_is_file(char* path) {
    int st[4];
    if (__sys_stat2(path, st) != 0) return 0;
    if (st[1]) return 0;
    return 1;
}
int pkg_exists(char* path) {
    int st[4];
    if (__sys_stat2(path, st) != 0) return 0;
    return 1;
}
int pkg_size(char* path) {
    int st[4];
    if (__sys_stat2(path, st) != 0) return -1;
    return st[0];
}

/* ---- helper path ---------------------------------------------- */
/* gabung direktori + nama -> out ("dir/name"); dir="" -> name saja */
void pkg_join(char* out, int cap, char* dir, char* name) {
    int i = 0;
    int k = 0;
    while (dir[k] && i < cap - 1) { out[i] = dir[k]; i = i + 1; k = k + 1; }
    if (i > 0 && out[i - 1] != '/' && i < cap - 1) { out[i] = '/'; i = i + 1; }
    k = 0;
    while (name[k] && i < cap - 1) { out[i] = name[k]; i = i + 1; k = k + 1; }
    out[i] = 0;
}
/* ambil komponen terakhir dari sebuah path */
void pkg_base(char* out, int cap, char* path) {
    int last = -1;
    int i = 0;
    int k = 0;
    while (path[i]) { if (path[i] == '/') last = i; i = i + 1; }
    i = 0;
    if (last >= 0) i = last + 1;
    while (path[i] && k < cap - 1) { out[k] = path[i]; i = i + 1; k = k + 1; }
    out[k] = 0;
}

/* ---- direktori ------------------------------------------------ */
/* mkdir -p: tiap level dibuat bila belum ada. 0 = ok. */
int pkg_mkdir_p(char* path) {
    char tmp[112];
    int n = 0;
    int i = 0;
    while (path[n] && n < 110) { tmp[n] = path[n]; n = n + 1; }
    tmp[n] = 0;
    if (tmp[0] == '/') i = 1;
    while (i < n) {
        while (i < n && tmp[i] != '/') i = i + 1;
        tmp[i] = 0;
        if (i > 0 && !pkg_is_dir(tmp)) __sys_mkdir1(tmp);
        tmp[i] = '/';
        i = i + 1;
    }
    return 0;
}

/* ---- salin ---------------------------------------------------- */
int pkg_copy_file(char* src, char* dst) {
    int fd = open(src);
    if (fd < 0) return fd;
    char* buf = malloc(PKG_BUF_CAP);
    if (!buf) { close(fd); return -5; }
    int total = 0;
    while (total + 4096 <= PKG_BUF_CAP) {
        int r = read(fd, buf + total, 4096);
        if (r <= 0) break;
        total = total + r;
    }
    close(fd);
    if (total <= 0) { free(buf); return -3; }
    int w = file_write(dst, buf, total);
    free(buf);
    return w;
}

/* salin path (berkas ATAU direktori rekursif) ke dst.
   dst berupa direktori yang sudah ada -> masuk dst/<base>. */
int pkg_copy_path(char* src, char* dst) {
    if (pkg_is_dir(src)) {
        char base[64];
        char target[112];
        char de[72];
        pkg_base(base, 64, src);
        pkg_join(target, 112, dst, base);
        pkg_mkdir_p(target);
        int fd = __sys_open2(src, PKG_O_DIR);
        if (fd < 0) return fd;
        while (__sys_readdir2(fd, de) == 1) {
            if (de[0] == '.' && de[1] == 0) continue;
            if (de[0] == '.' && de[1] == '.' && de[2] == 0) continue;
            char child[112];
            char tchild[112];
            int isd = (de[64] & 255) | ((de[65] & 255) << 8);
            pkg_join(child, 112, src, de);
            if (isd) {
                pkg_copy_path(child, target);
            } else {
                pkg_join(tchild, 112, target, de);
                pkg_copy_file(child, tchild);
            }
        }
        close(fd);
        return 0;
    }
    if (pkg_is_dir(dst)) {
        char base[64];
        char target[112];
        pkg_base(base, 64, src);
        pkg_join(target, 112, dst, base);
        return pkg_copy_file(src, target);
    }
    return pkg_copy_file(src, dst);
}
int pkg_copy(char* src, char* dst) { return pkg_copy_path(src, dst); }

/* ---- hapus (rekursif) — pkg_move butuh ini, jadi lebih dulu ---- */
int pkg_remove(char* path) {
    if (pkg_is_dir(path)) {
        int fd = __sys_open2(path, PKG_O_DIR);
        if (fd < 0) return fd;
        char de[72];
        while (__sys_readdir2(fd, de) == 1) {
            if (de[0] == '.' && de[1] == 0) continue;
            if (de[0] == '.' && de[1] == '.' && de[2] == 0) continue;
            char child[112];
            pkg_join(child, 112, path, de);
            pkg_remove(child);
        }
        close(fd);
        return __sys_rmdir1(path);
    }
    return __sys_unlink1(path);
}

/* ---- pindah ---------------------------------------------------- */
/* move = rename dulu; gagal -> masuk dst/<base>; gagal lagi ->
   salin lalu hapus sumber (fallback lintas lokasi). */
int pkg_move(char* src, char* dst) {
    if (__sys_rename2(src, dst) == 0) return 0;
    if (pkg_is_dir(dst)) {
        char base[64];
        char target[112];
        pkg_base(base, 64, src);
        pkg_join(target, 112, dst, base);
        if (__sys_rename2(src, target) == 0) return 0;
    }
    if (pkg_copy_path(src, dst) != 0) return -1;
    return pkg_remove(src);
}

/* ---- teks ------------------------------------------------------ */
int pkg_write(char* path, char* text) {
    int n = 0;
    while (text[n]) n = n + 1;
    return file_write(path, text, n);
}
int pkg_append(char* path, char* text) {
    char* buf = malloc(8192);
    if (!buf) return -5;
    int total = 0;
    int fd = open(path);
    if (fd >= 0) {
        while (total + 512 < 8192) {
            int r = read(fd, buf + total, 512);
            if (r <= 0) break;
            total = total + r;
        }
        close(fd);
    }
    int n = 0;
    while (text[n]) { buf[total + n] = text[n]; n = n + 1; }
    total = total + n;
    if (total >= 8192) { free(buf); return -6; }
    int w = file_write(path, buf, total);
    free(buf);
    return w;
}
/* baca teks -> out (NUL-terminated); balik jumlah byte / errno */
int pkg_read(char* path, char* out, int cap) {
    int fd = open(path);
    if (fd < 0) return fd;
    int total = 0;
    while (total + 1 < cap) {
        int r = read(fd, out + total, cap - 1 - total);
        if (r <= 0) break;
        total = total + r;
    }
    close(fd);
    out[total] = 0;
    return total;
}

/* ---- daftar ---------------------------------------------------- */
/* isi nama entri ke buffer FLAT (stride byte per slot, tiap slot
   di-NUL-kan). Tanpa "." dan "..". balik jumlah entri / errno. */
int pkg_list(char* dir, char* buf, int stride, int max) {
    int fd = __sys_open2(dir, PKG_O_DIR);
    if (fd < 0) return fd;
    char de[72];
    int n = 0;
    while (__sys_readdir2(fd, de) == 1 && n < max) {
        if (de[0] == '.' && de[1] == 0) continue;
        if (de[0] == '.' && de[1] == '.' && de[2] == 0) continue;
        int k = 0;
        char* slot = buf + n * stride;
        while (de[k] && k < stride - 1) { slot[k] = de[k]; k = k + 1; }
        slot[k] = 0;
        n = n + 1;
    }
    close(fd);
    return n;
}
/* jumlah entri (tanpa "." "..") / errno */
int pkg_count(char* dir) {
    int fd = __sys_open2(dir, PKG_O_DIR);
    if (fd < 0) return fd;
    char de[72];
    int n = 0;
    while (__sys_readdir2(fd, de) == 1) {
        if (de[0] == '.' && de[1] == 0) continue;
        if (de[0] == '.' && de[1] == '.' && de[2] == 0) continue;
        n = n + 1;
    }
    close(fd);
    return n;
}

/* manifest rekursif: daftar path berkas dipisah '\n' -> out.
   balik jumlah berkas / errno. */
int pkg_manifest(char* dir, char* out, int cap) {
    int fd = __sys_open2(dir, PKG_O_DIR);
    if (fd < 0) return fd;
    char de[72];
    int n = 0;
    int used = 0;
    while (__sys_readdir2(fd, de) == 1) {
        if (de[0] == '.' && de[1] == 0) continue;
        if (de[0] == '.' && de[1] == '.' && de[2] == 0) continue;
        char child[112];
        pkg_join(child, 112, dir, de);
        int isd = (de[64] & 255) | ((de[65] & 255) << 8);
        if (isd) {
            char sub[2048];
            int r = pkg_manifest(child, sub, 2048);
            if (r > 0) {
                int k = 0;
                while (sub[k] && used < cap - 2) { out[used] = sub[k]; used = used + 1; k = k + 1; }
                n = n + r;
            }
        } else {
            int k = 0;
            while (child[k] && used < cap - 2) { out[used] = child[k]; used = used + 1; k = k + 1; }
            if (used < cap - 1) { out[used] = '\n'; used = used + 1; }
            n = n + 1;
        }
    }
    close(fd);
    if (used < cap) out[used] = 0;
    return n;
}

/* ---- kemudahan khas package ------------------------------------- */
/* pasang satu artefak .mrp ke /bin (dibuat bila belum ada). 0 / errno */
int pkg_bin_install(char* mrp) {
    if (!pkg_is_dir("/bin")) __sys_mkdir1("/bin");
    char base[64];
    char target[112];
    pkg_base(base, 64, mrp);
    pkg_join(target, 112, "/bin", base);
    return pkg_copy_file(mrp, target);
}
/* catat ke installed.db sederhana: baris "<pkg> <file>" (append) */
int pkg_db_add(char* pkg, char* file) {
    char line[160];
    int i = 0;
    int k = 0;
    while (pkg[i] && i < 60) { line[i] = pkg[i]; i = i + 1; }
    line[i] = ' '; i = i + 1;
    while (file[k] && i < 150) { line[i] = file[k]; i = i + 1; k = k + 1; }
    line[i] = '\n'; i = i + 1;
    line[i] = 0;
    return pkg_append("/equinox/.local/installed.db", line);
}
#endif
