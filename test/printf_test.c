#include <stdio.h>
#include <stdlib.h>

int main(void) {
    fprintf(stderr, "printf works: %d + %d = %d\n", 2, 2, 2 + 2);

    int* p = malloc(64 * sizeof(int));
    for (int i = 0; i < 64; i++) p[i] = i * i;
    long sum = 0;
    for (int i = 0; i < 64; i++) sum += p[i];
    free(p);

    fprintf(stderr, "malloc/free works: sum = %ld\n", sum);
    return 0;
}
