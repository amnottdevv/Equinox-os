/* guess.c — interactive demo: readline + manual number parsing + loop. */
int main() {
    print("What is your name? ");
    char name[32];
    readline(name, 32);
    print("Hello, ");
    print(name);
    print("!\n");

    int secret = 7;
    int tries = 0;
    print("Guess a number 1..10 (press enter after each guess):\n");
    while (1) {
        char buf[16];
        int g = 0;
        readline(buf, 16);
        int k = 0;
        while (buf[k] >= '0' && buf[k] <= '9') {
            g = g * 10 + (buf[k] - '0');
            k++;
        }
        tries++;
        if (g == secret) break;
        if (g < secret) print("too small\n");
        else            print("too big\n");
    }
    print("Correct after ");
    printint(tries);
    print(" guesses. ");
    print(name);
    print(" wins!\n");
    return 0;
}
