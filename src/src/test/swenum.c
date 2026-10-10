/* Stage-1 test: enum constants + the switch statement (with fall-through,
   break, and default). */
#include <morph.h>

enum Color { RED, GREEN = 5, BLUE };

int grade(int score) {
    int acc = 0;
    switch (score) {
        case 1: acc = 10; break;
        case 2: acc = 20; break;
        case 3: acc = 30;
        case 4: acc = acc + 5; break;
        default: acc = -1; break;
    }
    return acc;
}

int main() {
    printf("RED=%d GREEN=%d BLUE=%d\n", RED, GREEN, BLUE);

    int acc = 0;
    int c = 2;
    switch (c) {
        case 9: acc = 900; break;
        case 2: acc = 222; break;
        default: acc = 777; break;
    }
    printf("sw=%d\n", acc);

    printf("g1=%d\n", grade(1));
    printf("g2=%d\n", grade(2));
    printf("g3=%d\n", grade(3));
    printf("g9=%d\n", grade(9));

    int day = 3;
    int weekend = 0;
    switch (day) {
        case 6:
        case 7: weekend = 1; break;
        default: weekend = 0; break;
    }
    printf("weekend=%d\n", weekend);

    return 0;
}
