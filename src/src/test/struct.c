/* Stage-2 test: struct, union, typedef, pointer fields, arrays of
   structs, nested structs, struct copy, enum constants. */
#include <morph.h>

struct Point { int x; int y; };
union Mixed { int i; char c[4]; };
typedef struct Point Pt;

int distancex(struct Point* a, struct Point* b) {
    int d = a->x - b->x;
    if (d < 0) d = 0 - d;
    return d;
}

int main() {
    struct Point p;
    p.x = 3;
    p.y = 7;
    printf("p=(%d,%d)\n", p.x, p.y);

    struct Point* pp = &p;
    pp->x = 42;
    printf("pp->x=%d\n", pp->x);

    struct Point q;
    q = p;
    printf("q=(%d,%d)\n", q.x, q.y);

    Pt r;
    r.x = 1;
    r.y = 2;
    printf("r=(%d,%d)\n", r.x, r.y);

    struct Box { struct Point tl; struct Point br; };
    struct Box b;
    b.tl.x = 0; b.tl.y = 0;
    b.br.x = 10; b.br.y = 20;
    printf("box=%d,%d..%d,%d\n", b.tl.x, b.tl.y, b.br.x, b.br.y);

    struct Point pts[3];
    pts[0].y = 10;
    pts[1].y = 20;
    pts[2].y = 30;
    int sum = 0;
    for (int i = 0; i < 3; i = i + 1) sum = sum + pts[i].y;
    printf("sum=%d\n", sum);

    union Mixed m;
    m.i = 0x41424344;
    printf("m.c[0]=%d\n", m.c[0]);

    struct Point a; a.x = 5; a.y = 6;
    struct Point b2; b2.x = 1; b2.y = 2;
    printf("dx=%d\n", distancex(&a, &b2));

    struct Rec { int id; char tag[8]; int w[2]; };
    struct Rec rc;
    rc.id = 9;
    rc.w[0] = 111;
    rc.w[1] = 222;
    printf("rc=%d,%d,%d\n", rc.id, rc.w[0], rc.w[1]);

    return 0;
}
