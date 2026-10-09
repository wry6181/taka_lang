// generated from mut.tk — do not edit
#include <stdint.h>
#include <stdio.h>

int main(void) {
    int8_t a = 8;  // taka: non-mut, checker would reject later mutation
    int8_t x = 32;
    a += 8;
    x += 2;
    int32_t *p = (int32_t*)&a;  // taka: non-mut, checker would reject later mutation
    int32_t *r = (int32_t*)&x;  // taka 14d: `mut` on `&` is a checker error
    printf("a=%d x=%d *p=%d r=%d\n", (int)a, (int)x, (int)*p, (int)(*r));
    return 0;
}
