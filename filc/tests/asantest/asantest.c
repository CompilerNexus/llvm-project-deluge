/* This is a test where ASAN does not detect a memory safety error, but Fil-C does. Let's make sure
   Fil-C always detects this error. */

#include <stdlib.h>
#include <inttypes.h>
#include <stdio.h>

__attribute__((__noinline__)) void foo(int* buf, uintptr_t distance)
{
    buf[distance] = 666;
}

int main()
{
    int* buf = malloc(100);
    int* buf2 = malloc(100);
    uintptr_t distance = buf2 - buf;
    foo(buf, distance);
    printf("%d\n", *buf2);
    free(buf);
    free(buf2);
    return 0;
}
