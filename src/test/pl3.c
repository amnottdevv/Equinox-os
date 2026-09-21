/* pl3.c — bisect: array LOKAL[100] + for + index variabel (bukan global) */
int main() {
    int i;
    int a[100];
    for (i = 0; i < 100; i++) a[i] = 1;
    printint(a[50]);
    print("\n");
    return 0;
}
