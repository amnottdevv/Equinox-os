/* hello.c — mtcc demo: compiling & executing C from INSIDE Equinox OS.
 * If this file runs, the self-hosting toolchain v0.1 is alive:
 *   run tcc.mrp hello.c        (compile & run, like tcc -run)
 *   run tcc.mrp -c hello.c     (compile to hello.mrp, standalone)
 */

/* Globals: char array + string init, int zero-init (data area zeroed) */
char greeting[32] = "mtcc runs in Equinox OS!\n";
int counter;

int fib(int n) {              /* recursion + if + value return */
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int add(int a, int b) {       /* 2-parameter function */
    return a + b;
}

int main() {
    print("Hello from C compiled inside Equinox OS!\n");

    printint(add(40, 2));     /* 42 — user function call */
    print("\n");
    printint(fib(12));        /* 144 — recursion */
    print("\n");

    counter = 0;              /* global scalar assignment */
    int i;
    for (i = 0; i < 5; i++) {
        counter += i;         /* compound assign on a global */
    }
    print("counter=");
    printint(counter);        /* 10 = 0+1+2+3+4 */
    print("\n");

    int arr[5];               /* local array + index lvalue */
    int j;
    for (j = 0; j < 5; j = j + 1) arr[j] = j * j;
    int sum = 0;
    for (j = 0; j < 5; j++) sum += arr[j];
    print("sum=");
    printint(sum);            /* 30 = 0+1+4+9+16 */
    print("\n");

    print(greeting);          /* global char[] decays to pointer */

    char* p = greeting;       /* pointer walk + post-inc + deref */
    int n = 0;
    while (*p) {
        n++;
        p++;
    }
    print("len(greeting)=");
    printint(n);              /* 25 */
    print("\n");

    printint((sum > 10) ? 0xFF : 0x10);   /* ternary + hex literal */
    print("\n");
    printint(1 << 10);        /* left shift: 1024 */
    print("\n");
    printint(7 / 2);          /* 3 */
    print("\n");
    printint(7 % 2);          /* 1 */
    print("\n");
    printint(100 >> 2);       /* 25 */
    print("\n");
    printint('A');            /* char literal = 65 */
    print("\n");
    return 0;
}
