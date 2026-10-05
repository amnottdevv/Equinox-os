#include <stdio.h>
int sumsq(int n) {
    int t = 0;
    for (int i = 1; i <= n; i++) t = t + i * i;
    return t;
}
int main() {
    printf("sumsq(10)=%d\n", sumsq(10));
    return 0;
}
