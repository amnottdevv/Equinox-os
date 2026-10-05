/* edge.c — mtcc edge-case codegen test: do-while, forward prototypes,
 * compound pointer assign, prefix ++/--, && || short-circuit, nested
 * ternary, global list & char* init, nested calls, break in while,
 * unary minus, shifts, else-if chain. */
int fact(int n);                       /* forward prototype (body below) */

int table[5] = { 3, 1, 4, 1, 5 };      /* global initializer list */
char* msg = "edge ok\n";               /* global char* to a literal */
char buf[12] = "abcd";                 /* char array string init */

int twice(int x) { return x * 2; }

int fact(int n) {                      /* body after the prototype */
    int r = 1;
    int i;
    for (i = 2; i <= n; i++) r = r * i;
    return r;
}

int main() {
    printint(fact(5));  print("\n");            /* 120 — forward call */

    int s = 0;
    int i = 0;
    do {                                         /* do-while */
        s += table[i];
        i++;
    } while (i < 5);
    printint(s);  print("\n");                  /* 3+1+4+1+5 = 14 */

    print(msg);                                 /* "edge ok" */

    int n = 0;
    while (n < 10) {                             /* break in while */
        if (n == 4) break;
        n++;
    }
    printint(n);  print("\n");                  /* 4 */

    int a = 10;
    int b = 0;
    if (a > 5 && b == 0) print("and1\n");       /* short-circuit && */
    if (b != 0 && 1 / b > 0) print("bug\n");    /* rhs must not be evaluated */
    else print("and2\n");
    if (a > 100 || b == 0) print("or1\n");      /* short-circuit || */

    printint(a > 5 ? (b == 0 ? 111 : 222) : 333);  /* ternary nested */
    print("\n");                                /* 111 */

    int arr[3];
    arr[0] = 7; arr[1] = 8; arr[2] = 9;
    int* p = arr;
    p += 2;                                      /* compound pointer */
    printint(*p);  print("\n");                 /* 9 */
    p = p - 2;
    printint(*p);  print("\n");                 /* 7 */
    printint(*p + 1);  print("\n");             /* 7+1 = 8 (not p+1) */

    int c = 5;
    printint(++c);  print("\n");                /* 6 */
    printint(c++);  print("\n");                /* 6 (post) */
    printint(c);  print("\n");                  /* 7 */
    printint(--c);  print("\n");                /* 6 */

    printint(twice(twice(3)));  print("\n");    /* 12 — nested call */

    if (-fact(3) == -6) print("negok\n");     /* signed negation (not
                                                  printint: that is unsigned!) */

    printint(1 << 3 | 1);  print("\n");         /* 9 */

    int x = 42;
    if (x < 0)       print("neg\n");
    else if (x == 0) print("zero\n");
    else             print("pos\n");            /* pos */

    print(buf);  print("\n");                  /* abcd */
    buf[0] = 'A';
    print(buf);  print("\n");                  /* Abcd */

    int k = 0;
    int total = 0;
    for (k = 0; k < 5; k++) {
        if (k == 2) continue;                   /* skip k=2 */
        total += table[k];
    }
    printint(total);  print("\n");              /* 3+1+1+5 = 10 */
    return 0;
}
