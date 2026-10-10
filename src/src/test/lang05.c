/* Stage-5 language test: sizeof and `static` locals.
   Both are compile-time/zero-init features, so every number below is
   derivable by hand from the mtcc type rules:
     - sizeof(char)=1, sizeof(int)=4, sizeof(any pointer)=4
     - struct fields are placed on a 4-byte boundary and the total is
       rounded up to 4 -> struct { char c; } is 4, not 1
     - a union is as big as its largest member (rounded to 4)
     - an array's sizeof is element count * element size (NOT rounded)
   A `static` local keeps its storage in the data area: initialized once,
   zero when not given an initializer, and it survives between calls. */
#include <morph.h>

struct Point { int x; int y; };     /* 8  */
struct Rec   { int a; char b[6]; }; /* 12 : a@0, b@4..9 -> round to 12 */
union  Mix   { int i; char c[4]; }; /* 4  */
struct Small { char c; };           /* 4  : 1 byte rounded to 4 */
typedef struct Point Pt;
typedef char* Name;

/* sizeof is a CONSTANT, so it is legal where a constant is required:
   an array bound and a static initializer. */
int gwords[sizeof(int)];            /* 4 ints -> 16 bytes */

int tick(void) {
    static int n = 100;    /* constant initializer, applied ONCE */
    n = n + 1;
    return n;
}

int zeroed(void) {
    static int acc;        /* no initializer -> the data area is zero */
    acc = acc + 7;
    return acc;
}

int main() {
    /* ---- sizeof (type-name) ---- */
    printf("sizeof(char)=%d\n", sizeof(char));
    printf("sizeof(int)=%d\n", sizeof(int));
    printf("sizeof(struct Point)=%d\n", sizeof(struct Point));
    printf("sizeof(struct Rec)=%d\n", sizeof(struct Rec));
    printf("sizeof(union Mix)=%d\n", sizeof(union Mix));
    printf("sizeof(struct Small)=%d\n", sizeof(struct Small));
    printf("sizeof(Pt)=%d\n", sizeof(Pt));
    printf("sizeof(struct Point*)=%d\n", sizeof(struct Point*));
    printf("sizeof(Name)=%d\n", sizeof(Name));

    /* ---- sizeof variable: arrays keep their real size ---- */
    int arr[5];
    char buf[10];
    Pt one;
    printf("sizeof(arr)=%d\n", sizeof arr);
    printf("sizeof(buf)=%d\n", sizeof buf);
    printf("sizeof(one)=%d\n", sizeof one);

    /* ---- sizeof in constant contexts ---- */
    char probe[sizeof(int)];        /* bound = 4 */
    probe[3] = 'z';
    printf("probe=%d %d\n", sizeof probe, probe[3]);
    printf("sizeof(gwords)=%d\n", sizeof gwords);
    static int csz = sizeof(struct Point);
    printf("csz=%d\n", csz);

    /* ---- static locals ---- */
    static struct Point sp = { 11, 22 };
    static char tbl[4];
    printf("sp=%d,%d\n", sp.x, sp.y);
    tbl[0] = 'A';
    printf("tbl=%d,%d,%d\n", tbl[0], tbl[1], tbl[2]);
    printf("sizeof(sp)=%d sizeof(tbl)=%d\n", sizeof sp, sizeof tbl);

    int t1 = tick();
    int t2 = tick();
    printf("tick=%d,%d\n", t1, t2);

    int z1 = zeroed();
    int z2 = zeroed();
    printf("zeroed=%d,%d\n", z1, z2);

    /* a static inside a nested block: block-scoped name, permanent store */
    {
        static int deep = 5;
        deep = deep + 1;
        printf("deep=%d\n", deep);
    }

    /* the ordinary local must still work after all of the above */
    int plain = 3;
    plain = plain + 4;
    printf("plain=%d\n", plain);
    return 0;
}
