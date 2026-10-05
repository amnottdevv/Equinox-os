/* pl5.c — bisect: array global[100] + for + index KONSTAN */
int a[100];

int main() {
    int i;
    for (i = 0; i < 100; i++) a[7] = 1;
    printint(a[7]);
    print("\n");
    return 0;
}
