// interp32.cpp - x86-32 interpreter to verify mtcc on a 64-bit host.
// ----------------------------------------------------------------------------
//  Kenapa interpreter, bukan eksekusi asli? Mesin test gak punya libc 32-bit
//  (gcc -m32 cannot link). The interpreter executes EXACTLY the set of
//  instructions mtcc emits (closed list, see the tcc.cpp header comment) -
//  an instruction outside the set = a codegen bug caught immediately
//  pesan jelas (offset + bytes). Semantik aritmetika mengikuti x86:
//  signed division truncate-ke-nol, shift mask &31, dst.
//
//  `int 0x80` traps are dispatched to host implementations that mimic
//  syscall kernel Equinox OS (syscall.cpp) — print ke stdout, readline dari
//  stdin, malloc from a virtual arena, etc. gettick = the instruction
//  (deterministik antar-run — penting utk test).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include "interp32.h"

// ---- register ----
enum { rEAX = 0, rECX = 1, rEDX = 2, rEBX = 3, rESP = 4, rEBP = 5 };
static uint32_t R[8];
static uint64_t EIP;

// ---- flags (the ones mtcc uses: ZF SF OF) ----
static int FZ, FS, FO;

// ---- virtual memory ----
static const uint8_t* m_code;    static uint64_t m_code_base; static uint32_t m_code_len;
static const uint8_t* m_data;    static uint64_t m_data_base; static uint32_t m_data_len;
static uint8_t  m_stack[0x40000]; static const uint64_t STACK_TOP = 0x880000;
static uint8_t  m_arena[0x80000]; static const uint64_t ARENA_BASE = 0xA0000
    ;static uint32_t arena_used;
static const uint64_t SENTINEL = 0xFFFF0000ull;

static uint64_t g_ninstr;
static int g_fatal;                 // abort -> stop the interpreter immediately
static int g_trace;                 // INTERP_TRACE=1 → log N instruksi pertama
static uint32_t g_trace_left;

static void abort_interp(const char* why, uint64_t at) {
    fprintf(stderr, "[interp32] ABORT: %s (eip=0x%llx)\n", why,
            (unsigned long long)at);
    g_fatal = 1;
}

// ---- host "VESA" framebuffer (game API tests, syscalls 20-25) ----
// Declared BEFORE mem_ptr so the region check can whitelist it: the
// fake linear framebuffer is an mmap MAP_32BIT block whose plain
// address is handed to guest code through fbinfo.addr, exactly like
// the real LFB. Lazy-initialized in the syscall section below.
#define HOST_FB_W 320
#define HOST_FB_H 240
#define HOST_FB_BPP 32
#define HOST_FB_PITCH (HOST_FB_W * 4)
#define HOST_FB_SIZE ((size_t)HOST_FB_W * HOST_FB_H * 4)
static uint8_t* host_fb = nullptr;

// decode an address to a pointer (which region?)
static uint8_t* mem_ptr(uint64_t addr, uint32_t len, int writable, const char* who) {
    if (addr >= m_code_base && addr + len <= m_code_base + m_code_len) {
        if (writable) { abort_interp("menulis ke region kode (bug!)", addr); return NULL; }
        return (uint8_t*)(m_code + (addr - m_code_base));
    }
    if (addr >= m_data_base && addr + len <= m_data_base + m_data_len)
        return (uint8_t*)(m_data + (addr - m_data_base));
    if (addr >= STACK_TOP - sizeof(m_stack) && addr + len <= STACK_TOP)
        return m_stack + (addr - (STACK_TOP - sizeof(m_stack)));
    if (addr >= ARENA_BASE && addr + len <= ARENA_BASE + sizeof(m_arena))
        return m_arena + (addr - ARENA_BASE);
    if (host_fb && addr >= (uint64_t)(uintptr_t)host_fb
                && addr + len <= (uint64_t)(uintptr_t)host_fb + HOST_FB_SIZE)
        return host_fb + (addr - (uint64_t)(uintptr_t)host_fb);
    (void)who;
    abort_interp("wild memory access (address outside every region)", addr);
    return NULL;
}
static int rd8(uint64_t a)            { uint8_t* p = mem_ptr(a, 1, 0, "rd8");  return p ? p[0] : -1; }
static uint32_t rd32(uint64_t a)      { uint8_t* p = mem_ptr(a, 4, 0, "rd32"); return p ? (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24)) : 0; }
static void wr32(uint64_t a, uint32_t v) {
    uint8_t* p = mem_ptr(a, 4, 1, "wr32");
    if (p) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
}
static void wr8(uint64_t a, uint8_t v) {
    uint8_t* p = mem_ptr(a, 1, 1, "wr8");
    if (p) p[0] = v;
}

// instruction fetch - must stay inside the code region
static uint8_t fetch8(void) {
    if (EIP < m_code_base || EIP >= m_code_base + m_code_len) {
        abort_interp("fetch outside the code region (jumped to data?)", EIP);
        return 0;
    }
    g_ninstr++;
    return m_code[EIP++ - m_code_base];
}
static uint32_t fetch32(void) {
    uint32_t v = (uint32_t)fetch8();
    v |= (uint32_t)fetch8() << 8;
    v |= (uint32_t)fetch8() << 16;
    v |= (uint32_t)fetch8() << 24;
    return v;
}

static void push32(uint32_t v) { R[rESP] -= 4; wr32(R[rESP], v); }
static uint32_t pop32(void)    { uint32_t v = rd32(R[rESP]); R[rESP] += 4; return v; }

static void set_zsf(uint32_t v) { FZ = (v == 0); FS = (int32_t)v < 0; }
static void flags_add(uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    set_zsf(r);
    FO = ((int32_t)a < 0 && (int32_t)b < 0 && (int32_t)r >= 0) ||
         ((int32_t)a >= 0 && (int32_t)b >= 0 && (int32_t)r < 0);
}
static void flags_sub(uint32_t a, uint32_t b) {   // a - b
    uint32_t r = a - b;
    set_zsf(r);
    FO = ((int32_t)a < 0 && (int32_t)b >= 0 && (int32_t)r >= 0) ||
         ((int32_t)a >= 0 && (int32_t)b < 0 && (int32_t)r < 0);
}

static int cond(int cc) {
    switch (cc & 0xF) {
        case 0: return 0;                         //  dummy (cc nibble 0, meaningless in this set)
        case 4: return FZ;                        //  jz / sete
        case 5: return !FZ;                       //  jnz / setne
        case 12: return FS != FO;                 //  jl  / setl
        case 13: return FS == FO;                 //  jge / setge
        case 14: return FZ || (FS != FO);         //  jle / setle
        case 15: return !FZ && (FS == FO);        //  jg  / setg
        default: return 0;
    }
}

// ---- syscall host (meniru semantik kernel/library/syscall.cpp) ----
static int g_exit_flag;
static int g_exit_code;
static int g_stdin_eof;

// ---- host RAMFS emulation (for the Morph.h file-API tests) ----
// A std::map<std::string, std::string> mirrors what fs_ram.cpp does in
// the kernel: mkfile/write replaces the whole file (create-or-overwrite),
// readfile/filesize/fileexists resolve a path exactly once. Paths are
// used verbatim ("/" prefix stripped like syscall_resolve does).
#include <map>
#include <string>
static std::map<std::string, std::string> g_ramfs;

// ---- host "VESA" framebuffer helpers (see the region decl above) ----
// The fake LFB lives in a lazily mmap'd MAP_32BIT block (same trick as
// os_alloc in mtcc's host glue) so its address fits the guest's 32-bit
// arithmetic. Guest code computes pixel addresses from fbinfo.addr and
// reads/writes them through rd8/wr8/rd32/wr32, which now whitelist the
// region in mem_ptr — the fake fb behaves exactly like the real LFB.
static void host_fb_init(void) {
    if (host_fb) return;
    void* p = mmap(NULL, HOST_FB_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "[interp32] host fb mmap failed\n");
        exit(1);
    }
    host_fb = (uint8_t*)p;
    memset(host_fb, 0, HOST_FB_SIZE);
}
static void host_fb_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= HOST_FB_W || y >= HOST_FB_H) return;   // clip like vesa_draw_pixel
    uint32_t* px = (uint32_t*)(host_fb + (size_t)y * HOST_FB_PITCH + x * 4);
    *px = color;
}

// Host fd table: slots 3..15 map to RAMFS paths (like the kernel's
// sys_fd_entry, sequential pos read).
struct HostFd { std::string path; uint32_t pos; bool used; };
static HostFd g_fds[16];

static std::string ramfs_norm(const char* p) {
    std::string s(p ? p : "");
    if (!s.empty() && s[0] == '/') s.erase(0, 1);
    return s;
}
static bool ramfs_find(const char* p, std::string** out) {
    std::string key = ramfs_norm(p);
    auto it = g_ramfs.find(key);
    if (it == g_ramfs.end()) return false;
    *out = &it->second;
    return true;
}

static uint32_t sys_print(uint32_t p) {
    // print a C-string up to NUL (read8 so it is bounds-checked)
    uint32_t n = 0;
    for (;;) {
        int c = rd8(p + n);
        if (c <= 0) break;
        fputc(c, stdout);
        n++;
        if (n > 65536) { abort_interp("print: string without NUL", p); break; }
    }
    return 0;
}
static uint32_t sys_readline(uint32_t buf, uint32_t maxlen) {
    if (maxlen == 0) return 0;
    if (g_stdin_eof) { abort_interp("readline with stdin already exhausted", 0); return 0; }
    char line[1024];
    if (!fgets(line, (int)(maxlen < sizeof(line) ? maxlen : sizeof(line)), stdin)) {
        g_stdin_eof = 1;
        abort_interp("readline: stdin exhausted (test needs more input)", 0);
        return 0;
    }
    uint32_t n = (uint32_t)strlen(line);
    while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
    for (uint32_t i = 0; i <= n; i++) wr8(buf + i, (uint8_t)line[i]);  // termasuk NUL
    return n;
}

static uint32_t do_syscall(void) {
    uint32_t num = R[rEAX], a1 = R[rEBX], a2 = R[rECX], a3 = R[rEDX];
    switch (num) {
        case 1:  g_exit_flag = 1; g_exit_code = (int)a1; return a1;        // exit
        case 3:  return 1;                                                  // getpid
                case 4: { // write(fd, buf, len) - console only
            if (a1 == 1 || a1 == 2) {
                for (uint32_t i = 0; i < a3; i++) { int c = rd8(a2 + i); fputc(c, stdout); }
                return a3;
            }
            return (uint32_t)-2;
        }
        case 2:  return (uint32_t)-9;                                                                                // exec -> EBUSY: the kernel model after fix V3 #1 (nested exec refused)

        // ---- file API (mirrors kernel syscall.cpp RAMFS semantics) ----
        case 6: { // open(path) -> fd 3..15, atau -3 ENOENT
            char path[256];
            for (int i = 0; i < 255; i++) { path[i] = (char)rd8(a1 + i); if (!path[i]) break; }
            path[255] = 0;
            std::string* f;
            if (!ramfs_find(path, &f)) return (uint32_t)-3;
            for (int fd = 3; fd < 16; fd++) {
                if (!g_fds[fd].used) {
                    g_fds[fd].used = true;
                    g_fds[fd].path  = ramfs_norm(path);
                    g_fds[fd].pos   = 0;
                    return (uint32_t)fd;
                }
            }
            return (uint32_t)-8;                                            // EMFILE
        }
        case 5: { // read(fd, buf, len) — sequential, 0 = EOF
            if (a1 < 3 || a1 >= 16 || !g_fds[a1].used) return (uint32_t)-2;
            std::string& f = g_ramfs[g_fds[a1].path];
            uint32_t pos = g_fds[a1].pos;
            if (pos >= f.size()) return 0;
            uint32_t n = (uint32_t)f.size() - pos;
            if (n > a3) n = a3;
            for (uint32_t i = 0; i < n; i++) wr8(a2 + i, (uint8_t)f[pos + i]);
            g_fds[a1].pos = pos + n;
            return n;
        }
        case 7: { // close(fd)
            if (a1 < 3 || a1 >= 16 || !g_fds[a1].used) return (uint32_t)-2;
            g_fds[a1].used = false;
            g_fds[a1].path.clear();
            g_fds[a1].pos = 0;
            return 0;
        }
        case 16: { // mkfile(path, buf, len) — create-or-OVERWRITE (binary-safe)
            char path[256];
            for (int i = 0; i < 255; i++) { path[i] = (char)rd8(a1 + i); if (!path[i]) break; }
            path[255] = 0;
            if (!path[0] || !a2 || !a3) return (uint32_t)-6;
            std::string& f = g_ramfs[ramfs_norm(path)];
            f.assign(a3, '\0');
            for (uint32_t i = 0; i < a3; i++) f[i] = (char)rd8(a2 + i);
            return 0;
        }
        case 17: { // readfile(path, buf, maxlen) — whole file in one call
            char path[256];
            for (int i = 0; i < 255; i++) { path[i] = (char)rd8(a1 + i); if (!path[i]) break; }
            path[255] = 0;
            std::string* f;
            if (!ramfs_find(path, &f)) return (uint32_t)-3;
            uint32_t n = ((uint32_t)f->size() < a3) ? (uint32_t)f->size() : a3;
            for (uint32_t i = 0; i < n; i++) wr8(a2 + i, (uint8_t)(*f)[i]);
            return n;
        }
        case 18: { // filesize(path)
            char path[256];
            for (int i = 0; i < 255; i++) { path[i] = (char)rd8(a1 + i); if (!path[i]) break; }
            path[255] = 0;
            std::string* f;
            if (!ramfs_find(path, &f)) return (uint32_t)-3;
            return (uint32_t)f->size();
        }
        case 19: { // fileexists(path) — 1 / 0
            char path[256];
            for (int i = 0; i < 255; i++) { path[i] = (char)rd8(a1 + i); if (!path[i]) break; }
            path[255] = 0;
            std::string* unused;
            return ramfs_find(path, &unused) ? 1u : 0u;
        }

        case 8: { // getkey — blocking satu char dari stdin
            int c = g_stdin_eof ? EOF : getchar();
            if (c == EOF) { g_stdin_eof = 1; return (uint32_t)-1; }
            return (uint32_t)c;
        }
        case 9:  return sys_readline(a1, a2);
        case 10: return sys_print(a1);
        case 11: printf("%u", a1); return 0;                                // printint (unsigned!)
        case 12: { // malloc dari arena virtual
            uint32_t sz = (a1 + 3u) & ~3u;
            if (sz == 0 || arena_used + sz > sizeof(m_arena)) return 0;
            uint32_t p = (uint32_t)(ARENA_BASE + arena_used);
            arena_used += sz;
            return p;
        }
        case 13: return (uint32_t)g_ninstr;                                 // gettick (deterministik)
        case 14: return 0;                                                  // sleep (no-op)
        case 15: return 0;                                                  // getargs (empty)

        // ---- game API (mirrors kernel syscalls 20-25) ----
        case 20: { // fbinfo(info*) — fill morph_fbinfo_t for the fake fb
            if (!a1) return (uint32_t)-6;
            host_fb_init();
            wr32(a1 + 0,  (uint32_t)(uintptr_t)host_fb);   // addr
            wr32(a1 + 4,  HOST_FB_W);
            wr32(a1 + 8,  HOST_FB_H);
            wr32(a1 + 12, HOST_FB_BPP);
            wr32(a1 + 16, HOST_FB_PITCH);
            wr32(a1 + 20, 1);                              // avail = 1
            return 0;
        }
        case 21: { // putpixel(x, y, color)
            host_fb_init();
            host_fb_putpixel(a1, a2, a3);
            return 0;
        }
        case 22: { // fillrect(x|w<<16, y|h<<16, color)
            host_fb_init();
            uint32_t x = a1 & 0xFFFF, w = a1 >> 16;
            uint32_t y = a2 & 0xFFFF, h = a2 >> 16;
            for (uint32_t r = 0; r < h; r++)
                for (uint32_t c = 0; c < w; c++)
                    host_fb_putpixel(x + c, y + r, a3);
            return 0;
        }
        case 23: return 0;                                                  // pollkey: no key in tests
        case 24: { // mouse(state*) — center position, no buttons
                   // (kernel reports the screen center before any
                   //  movement — mirror that so tests match in-OS)
            if (!a1) return (uint32_t)-6;
            wr32(a1 + 0, HOST_FB_W / 2);
            wr32(a1 + 4, HOST_FB_H / 2);
            wr32(a1 + 8, 0);
            return 0;
        }
        case 25: return 0;                                                  // speaker(freq): silence in tests
        case 26: { // sndbeep(freq, ms) — emulate the KERNEL note queue
                   // semantics: ms==0 -> EINVAL(-6); 64 pending notes
                   // max -> EBUSY(-9). Playback itself is not emulated
                   // (the host has no PC speaker and no timer IRQ), so
                   // `pending` only grows within one process — tests
                   // run each .mrp in a fresh interpreter process.
            static uint32_t s_pending = 0;
            if (a2 == 0) return (uint32_t)-6;           // EINVAL
            if (s_pending >= 64) return (uint32_t)-9;   // EBUSY: queue full
            s_pending++;
            return 0;
        }

        // ---- v10.8 libc layer ----
        case 27: { // lseek(fd, off, whence) — mirrors kernel sys_lseek
            if (a1 < 3 || a1 >= 16 || !g_fds[a1].used) return (uint32_t)-2;
            std::string& f = g_ramfs[g_fds[a1].path];
            if (a3 > 2) return (uint32_t)-6;                                // EINVAL
            int32_t so = (int32_t)a2;
            int64_t target;
            if (a3 == 0)      target = (int64_t)so;
            else if (a3 == 1) target = (int64_t)g_fds[a1].pos + (int64_t)so;
            else              target = (int64_t)f.size() + (int64_t)so;
            if (target < 0) target = 0;
            if (target > (int64_t)f.size()) target = (int64_t)f.size();
            g_fds[a1].pos = (uint32_t)target;
            return g_fds[a1].pos;
        }
        case 28: { // printf(fmt, int args[3]) — mirror of kernel sys_printf
                   // render: %d %u %x %X %c %s %% with width/zero-pad/left-
                   // align; conversions beyond the 3 arg slots print (n/a).
                   // %s pointers read from interpreter memory (rd8).
            char kfmt[192];
            uint32_t n = 0;
            while (rd8(a1 + n) && n < sizeof(kfmt) - 1) { kfmt[n] = (char)rd8(a1 + n); n++; }
            kfmt[n] = 0;
            int kargs[3] = { 0, 0, 0 };
            if (a2) {
                kargs[0] = (int)rd32(a2 + 0);
                kargs[1] = (int)rd32(a2 + 4);
                kargs[2] = (int)rd32(a2 + 8);
            }
            uint32_t out = 0;
            int argn = 0;
            for (const char* p = kfmt; *p; p++) {
                if (*p != '%') { putchar(*p); out++; continue; }
                p++;
                if (*p == '%') { putchar('%'); out++; continue; }
                if (*p == '\0') break;
                int zero_pad = 0, width = 0, left_align = 0;
                if (*p == '-') { left_align = 1; p++; }
                if (*p == '0') { zero_pad = 1; p++; }
                while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
                char spec = *p;
                if (spec != 'd' && spec != 'u' && spec != 'x' && spec != 'X' &&
                    spec != 'c' && spec != 's') {
                    putchar('%'); putchar(spec ? spec : '?'); out += 2;
                    continue;
                }
                if (argn >= 3) { printf("(n/a)"); out += 5; argn++; continue; }
                int v = kargs[argn++];
                if (spec == 's') {
                    char buf[600];
                    uint32_t k = 0;
                    if (v == 0) { snprintf(buf, sizeof(buf), "%s", "(null)"); }
                    else {
                        while (k < sizeof(buf) - 1) {
                            char ch = (char)rd8((uint32_t)v + k);
                            if (!ch) break;
                            buf[k++] = ch;
                        }
                        buf[k] = 0;
                    }
                    uint32_t len = (uint32_t)strlen(buf);
                    if (!left_align)
                        for (uint32_t i = len; i < (uint32_t)width; i++) { putchar(' '); out++; }
                    for (uint32_t i = 0; i < len; i++) { putchar(buf[i]); out++; }
                    if (left_align)
                        for (uint32_t i = len; i < (uint32_t)width; i++) { putchar(' '); out++; }
                    continue;
                }
                if (spec == 'c') {
                    char c = (char)v;
                    if (!left_align) for (uint32_t i = 1; i < (uint32_t)width; i++) { putchar(' '); out++; }
                    putchar(c); out++;
                    if (left_align) for (uint32_t i = 1; i < (uint32_t)width; i++) { putchar(' '); out++; }
                    continue;
                }
                char nb[16];
                uint32_t nl = 0;
                uint32_t base = (spec == 'x' || spec == 'X') ? 16u : 10u;
                uint32_t uv;
                int negv = 0;
                if (spec == 'd' && v < 0) { negv = 1; uv = (uint32_t)(-(int32_t)v); }
                else uv = (uint32_t)v;
                if (uv == 0) nb[nl++] = '0';
                while (uv) {
                    uint32_t d = uv % base;
                    nb[nl++] = (char)(d < 10 ? '0' + d : (spec == 'X' ? 'A' : 'a') + (d - 10));
                    uv /= base;
                }
                if (negv) nb[nl++] = '-';
                char padc = (zero_pad && !negv && !left_align) ? '0' : ' ';
                if (!left_align)
                    for (uint32_t i = nl; i < (uint32_t)width; i++) { putchar(padc); out++; }
                for (int32_t i = (int32_t)nl - 1; i >= 0; i--) { putchar(nb[i]); out++; }
                if (left_align)
                    for (uint32_t i = nl; i < (uint32_t)width; i++) { putchar(' '); out++; }
            }
            return out;
        }
        case 29: return 3;                                                    // ringinfo: user program (CPL 3)
        default: fprintf(stderr, "[interp32] syscall asing %u\n", num); return (uint32_t)-1;
    }
}

// ---- modrm decoder for the forms mtcc emits ----
struct ModRM {
    int mod, reg, rm;
    uint64_t disp;      // effective when mod=10 / rm=101 (moffs)
    int is_moffs;
};
static ModRM decode_modrm(void) {
    uint8_t m = fetch8();
    ModRM d;
    d.mod = (m >> 6) & 3;
    d.reg = (m >> 3) & 7;
    d.rm  = m & 7;
    d.disp = 0;
    d.is_moffs = 0;
    if (d.mod == 0 && d.rm == 5) { d.is_moffs = 1; d.disp = fetch32(); }
    else if (d.mod == 1) d.disp = (int8_t)fetch8();
    else if (d.mod == 2) d.disp = (int32_t)fetch32();
    return d;
}
static uint64_t rm_addr(const ModRM& d) {
    if (d.is_moffs) return d.disp;
    if (d.mod == 0 && d.rm == 4) { abort_interp("SIB not in the mtcc set", EIP); return 0; }
    uint64_t base = (d.rm == 5) ? R[rEBP] : R[d.rm];    // rm=101 & mod=0 is already moffs
    return base + d.disp;
}

extern "C" int interp32_run(const uint8_t* code, uint64_t code_base, uint32_t code_len,
                            const uint8_t* data, uint64_t data_base, uint32_t data_len,
                            uint64_t entry,
                            int* exit_code, uint64_t* ninstr) {
    m_code = code; m_code_base = code_base; m_code_len = code_len;
    m_data = data; m_data_base = data_base; m_data_len = data_len;
    memset(R, 0, sizeof(R));
    memset(m_stack, 0, sizeof(m_stack));
    memset(m_arena, 0, sizeof(m_arena));
    arena_used = 0;
    g_ninstr = 0; g_exit_flag = 0; g_exit_code = 0; g_stdin_eof = 0;
    g_fatal = 0;
    {
        const char* tv = getenv("INTERP_TRACE");
        g_trace = tv && *tv && *tv != '0';
        g_trace_left = g_trace ? (tv && *tv >= '1' && *tv <= '9' ? (uint32_t)atoi(tv) : 400u) : 0;
    }
    FZ = FS = FO = 0;

    R[rESP] = (uint32_t)STACK_TOP;
    push32((uint32_t)SENTINEL);
    EIP = entry;

    const uint64_t BUDGET = 500000000ull;

    for (;;) {
        if (g_ninstr > BUDGET) { abort_interp("instruction budget exhausted (infinite loop?)", EIP); return 1; }
        if (g_fatal) return 1;
        if (g_exit_flag) { break; }

        uint64_t pc0 = EIP;
        uint8_t op = fetch8();
        if (g_trace && g_trace_left > 0) {
            g_trace_left--;
            fprintf(stderr, "  tr %04llx: %02x | eax=%08x ebx=%08x ecx=%08x edx=%08x esp=%08x ebp=%08x\n",
                    (unsigned long long)(pc0 - m_code_base), op,
                    R[0], R[3], R[1], R[2], R[4], R[5]);
        }
        if (g_fatal) return 1;
        switch (op) {

        // ---- push/pop GPR ----
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            push32(R[op - 0x50]); break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            R[op - 0x58] = pop32(); break;

        // ---- mov r32, imm32 ----
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            R[op - 0xB8] = fetch32(); break;

        // ---- mov eax, [moffs32] ----
        case 0xA1: {
            uint32_t a = fetch32();
            R[rEAX] = rd32(a);
            break;
        }

        // ---- mov r/m <- r  (89 /r) ----
        case 0x89: {
            ModRM d = decode_modrm();
            if (d.mod == 3) R[d.rm] = R[d.reg];
            else wr32(rm_addr(d), R[d.reg]);
            break;
        }
        // ---- mov r <- r/m  (8B /r) ----
        case 0x8B: {
            ModRM d = decode_modrm();
            if (d.mod == 3) R[d.reg] = R[d.rm];
            else R[d.reg] = rd32(rm_addr(d));
            break;
        }
        // ---- lea r32, [r/m]  (8D /r) - only the [ebp+disp] form mtcc emits ----
        case 0x8D: {
            ModRM d = decode_modrm();
            R[d.reg] = (uint32_t)rm_addr(d);
            break;
        }

        // ---- ALU r/m <- r : add/or/and/xor/sub/cmp ----
        case 0x01: case 0x09: case 0x21: case 0x31: case 0x29: case 0x39: {
            ModRM d = decode_modrm();
            uint32_t a, b = R[d.reg];
            if (d.mod == 3) a = R[d.rm]; else a = rd32(rm_addr(d));
            switch (op) {
                case 0x01: { uint32_t r = a + b; flags_add(a, b); if (d.mod==3) R[d.rm]=r; else wr32(rm_addr(d), r); break; }
                case 0x09: { uint32_t r = a | b; set_zsf(r); if (d.mod==3) R[d.rm]=r; else wr32(rm_addr(d), r); break; }
                case 0x21: { uint32_t r = a & b; set_zsf(r); if (d.mod==3) R[d.rm]=r; else wr32(rm_addr(d), r); break; }
                case 0x31: { uint32_t r = a ^ b; set_zsf(r); if (d.mod==3) R[d.rm]=r; else wr32(rm_addr(d), r); break; }
                case 0x29: { uint32_t r = a - b; flags_sub(a, b); if (d.mod==3) R[d.rm]=r; else wr32(rm_addr(d), r); break; }
                case 0x39: flags_sub(a, b); break;   // cmp - result discarded
            }
            break;
        }

        // ---- test eax, eax (85 /r mod=11) ----
        case 0x85: {
            ModRM d = decode_modrm();
            uint32_t r = R[d.reg] & R[d.rm];
            set_zsf(r); FO = 0;
            break;
        }

        // ---- imul eax, ebx (0F AF /r) ----
        case 0x0F: {
            uint8_t sub = fetch8();
            if (sub == 0xAF) {
                ModRM d = decode_modrm();
                uint32_t b = (d.mod == 3) ? R[d.rm] : rd32(rm_addr(d));
                uint64_t p = (uint64_t)(int64_t)(int32_t)R[d.reg] * (int64_t)(int32_t)b;
                uint32_t r = (uint32_t)p;
                R[d.reg] = r;
                set_zsf(r);
                FO = (p != (uint64_t)(int64_t)(int32_t)r);
            } else if (sub == 0xB6) {          // movzx r32, byte r/m
                ModRM d = decode_modrm();
                uint32_t v = (d.mod == 3) ? (R[d.rm] & 0xFF) : (uint32_t)rd8(rm_addr(d));
                R[d.reg] = v;
                        } else if ((sub & 0xF0) == 0x90) { // setcc r/m8 - the condition lives in
                // NIBBLE OPCODE (sub&0xF), BUKAN reg field modrm (3 bit —
                                // cc signed 12..15 don't fit!). Classic bug: setl always 0.
                ModRM d = decode_modrm();
                uint8_t v = cond(sub & 0xF) ? 1 : 0;
                if (d.mod == 3) R[d.rm] = (R[d.rm] & 0xFFFFFF00u) | v;
                else wr8(rm_addr(d), v);
            } else if ((sub & 0xF0) == 0x80) { // jcc rel32
                int cc = sub & 0xF;
                int32_t rel = (int32_t)fetch32();
                if (cond(cc)) EIP = (uint64_t)((int64_t)EIP + rel);
            } else {
                abort_interp("0F sub-opcode outside the mtcc set", pc0);
                return 1;
            }
            break;
        }

        // ---- imul reg, r/m, imm8 (6B /r ib) ----
        case 0x6B: {
            ModRM d = decode_modrm();
            uint32_t b = (d.mod == 3) ? R[d.rm] : rd32(rm_addr(d));
            int8_t imm = (int8_t)fetch8();
            uint32_t r = (uint32_t)((int64_t)(int32_t)b * (int64_t)imm);
            R[d.reg] = r;
            set_zsf(r);
            break;
        }

        // ---- cdq ----
        case 0x99:
            R[rEDX] = ((int32_t)R[rEAX] < 0) ? 0xFFFFFFFFu : 0u;
            break;

        // ---- F7 /r : neg/not/idiv ----
        case 0xF7: {
            ModRM d = decode_modrm();
            uint32_t a = (d.mod == 3) ? R[d.rm] : rd32(rm_addr(d));
            switch (d.reg) {
                case 2: { uint32_t r = ~a; set_zsf(r); if (d.mod==3) R[d.rm]=r; else wr32(rm_addr(d), r); break; }   // not
                case 3: { uint32_t r = (uint32_t)(-(int32_t)a); set_zsf(r); FO = (a == 0x80000000u); if (d.mod==3) R[d.rm]=r; else wr32(rm_addr(d), r); break; } // neg
                case 7: { // idiv ebx
                    int32_t dividend = (int32_t)((uint64_t)R[rEDX] << 32 | R[rEAX]);
                    int32_t divisor = (int32_t)a;
                    if (divisor == 0) { abort_interp("bagi nol (di kernel = #DE panic!)", pc0); return 1; }
                    if (divisor == -1 && dividend == (int32_t)0x80000000) { abort_interp("idiv overflow", pc0); return 1; }
                    R[rEAX] = (uint32_t)(dividend / divisor);
                    R[rEDX] = (uint32_t)(dividend % divisor);
                    break;
                }
                default:
                    abort_interp("F7 reg outside the mtcc set", pc0);
                    return 1;
            }
            break;
        }

        // ---- D3 /r : shl/shr/sar eax, cl ----
        case 0xD3: {
            ModRM d = decode_modrm();
            uint32_t a = (d.mod == 3) ? R[d.rm] : rd32(rm_addr(d));
            uint32_t sh = R[rECX] & 31;
            uint32_t r;
            switch (d.reg) {
                case 4: r = a << sh; break;                          // shl
                case 5: r = a >> sh; break;                          // shr
                case 7: r = (uint32_t)(((int32_t)a) >> sh); break;   // sar
                default: abort_interp("D3 reg outside the mtcc set", pc0); return 1;
            }
            set_zsf(r);
            if (d.mod == 3) R[d.rm] = r; else wr32(rm_addr(d), r);
            break;
        }

        // ---- jmp rel32 ----
        case 0xE9: {
            int32_t rel = (int32_t)fetch32();
            EIP = (uint64_t)((int64_t)EIP + rel);
            break;
        }
        // ---- call rel32 ----
        case 0xE8: {
            int32_t rel = (int32_t)fetch32();
            push32((uint32_t)EIP);
            EIP = (uint64_t)((int64_t)EIP + rel);
            break;
        }
        // ---- ret ----
        case 0xC3: {
            uint32_t r = pop32();
            if (r == (uint32_t)SENTINEL) goto done;   // return to the caller
            EIP = r;
            break;
        }

        // ---- int 0x80 ----
        case 0xCD: {
            uint8_t vec = fetch8();
            if (vec != 0x80) { abort_interp("int vector != 0x80", pc0); return 1; }
            R[rEAX] = do_syscall();
            break;
        }

        // ---- 81 EC imm32 (sub esp) / 83 EC|C4 imm8 ----
        case 0x81: {
            ModRM d = decode_modrm();
            if (d.reg != 5) { abort_interp("81 /reg outside the mtcc set", pc0); return 1; }
            uint32_t imm = fetch32();
            uint32_t a = (d.mod == 3) ? R[d.rm] : rd32(rm_addr(d));
            uint32_t r = a - imm;
            flags_sub(a, imm);
            if (d.mod == 3) R[d.rm] = r; else wr32(rm_addr(d), r);
            break;
        }
        case 0x83: {
            ModRM d = decode_modrm();
            int8_t imm = (int8_t)fetch8();
            uint32_t a = (d.mod == 3) ? R[d.rm] : rd32(rm_addr(d));
            uint32_t r;
            switch (d.reg) {
                case 5: r = a - (uint32_t)(int32_t)imm; flags_sub(a, (uint32_t)(int32_t)imm); break;  // sub
                case 0: r = a + (uint32_t)(int32_t)imm; flags_add(a, (uint32_t)(int32_t)imm); break;  // add
                default: abort_interp("83 /reg outside the mtcc set", pc0); return 1;
            }
            if (d.mod == 3) R[d.rm] = r; else wr32(rm_addr(d), r);
            break;
        }

        // ---- FF C0/C8 : inc/dec eax ----
        case 0xFF: {
            ModRM d = decode_modrm();
            uint32_t a = (d.mod == 3) ? R[d.rm] : rd32(rm_addr(d));
            uint32_t r;
            if (d.reg == 0)      { r = a + 1; set_zsf(r); }
            else if (d.reg == 1) { r = a - 1; set_zsf(r); }
            else { abort_interp("FF /reg outside the mtcc set", pc0); return 1; }
            if (d.mod == 3) R[d.rm] = r; else wr32(rm_addr(d), r);
            break;
        }

        // ---- 88 /r : mov byte r/m <- al ----
        case 0x88: {
            ModRM d = decode_modrm();
            uint8_t v = (uint8_t)(R[d.reg] & 0xFF);
            if (d.mod == 3) R[d.rm] = (R[d.rm] & 0xFFFFFF00u) | v;
            else wr8(rm_addr(d), v);
            break;
        }

        default:
            fprintf(stderr, "[interp32] ABORT: opcode 0x%02X outside the mtcc set @0x%llx\n",
                    op, (unsigned long long)pc0);
            return 1;
        }
    }
done:
    if (exit_code) *exit_code = g_exit_flag ? g_exit_code : (int)R[rEAX];
    if (ninstr) *ninstr = g_ninstr;
    return 0;
}
