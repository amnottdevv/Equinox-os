/* primes.c — sieve of Eratosthenes: global array + nested loop +
 * break + continue + return value from main. */
int sieve[100];

int main() {
    int i;
    for (i = 0; i < 100; i++) sieve[i] = 1;
    sieve[0] = 0;
    sieve[1] = 0;

    int n;
    for (n = 2; n < 100; n++) {
        if (!sieve[n]) continue;      /* already marked composite */
        if (n * n >= 100) break;      /* the rest are guaranteed prime */
        int m;
        for (m = n * n; m < 100; m = m + n) sieve[m] = 0;
    }

    int count = 0;
    for (i = 2; i < 100; i++) {
        if (sieve[i]) {
            printint(i);
            print(" ");
            count++;
        }
    }
    print("\n");
    print("count: ");
    printint(count);
    print("\n");
    return count;                     /* exit code = number of primes < 100 */
}
