/* pl2.c — bisect: array global[10] (bukan 100) + for + index variabel */
int a[10];

int main() {
    int i;
    for (i = 0; i < 10; i++) a[i] = 1;
    printint(a[5]);
    print("\n");
    return 0;
}
