/* sinit.c — struct/union INITIALIZER regression (global + local + nested).
 *
 * Covers the { } walker added for Stage 3:
 *   - global scalar struct      struct Point g = {10, 20};
 *   - local  scalar struct      struct Point p = {5, 6};
 *   - nested struct (global)    struct Line  l = { {1,2}, {3,4}, 7 };
 *   - nested struct (local)     struct Line  loc = { {7,8}, {9,10}, 11 };
 *   - array of struct           struct Point arr[3] = { {1,1}, ... };
 *   - union (first member only) union Num u = {99};
 *   - char fields               stored/reloaded as single bytes
 *   - char* field = "literal"   slot holds the literal's address
 *
 * Layout rule (matches the struct body parser): struct fields are 4-aligned,
 * union fields all sit at offset 0.
 */
#include <morph.h>

struct Point { int x; int y; };
struct Line  { struct Point a; struct Point b; int tag; };
union  Num   { int i; char c; };
struct Mix   { char c; int n; char d; int m; };
struct Ptr   { char* s; int k; };

struct Point g    = {10, 20};
struct Line  l    = { {1, 2}, {3, 4}, 7 };
union  Num   u    = {99};
struct Point arr[3] = { {1, 1}, {2, 2}, {3, 3} };
struct Mix   mix  = {65, 7, 66, 8};
struct Ptr   hp   = { "hi", 3 };

int main(void) {
    struct Point p   = {5, 6};
    struct Line  loc = { {7, 8}, {9, 10}, 11 };
    struct Mix   m   = {67, 9, 68, 10};

    print("g=");
    printint(g.x); print(","); printint(g.y);
    print(" p=");
    printint(p.x); print(","); printint(p.y);
    print(" l=");
    printint(l.a.y); print(","); printint(l.b.x); print(","); printint(l.tag);
    print(" loc=");
    printint(loc.a.x); print(","); printint(loc.b.y); print(","); printint(loc.tag);
    print(" arr=");
    printint(arr[0].x); print(","); printint(arr[2].y);
    print(" u=");
    printint(u.i);
    print("\n");
    print("mix=");
    printint(mix.c); print(","); printint(mix.n); print(","); printint(mix.d); print(","); printint(mix.m);
    print(" m=");
    printint(m.c); print(","); printint(m.n); print(","); printint(m.d); print(","); printint(m.m);
    print(" hp=");
    print(hp.s); printint(hp.k);
    print("\n");
    return 0;
}
