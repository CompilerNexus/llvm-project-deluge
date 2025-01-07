#include <string.h>
#include <stdfil.h>

int main()
{
    int x[100];
    int y[1000];
    unsigned i;
    for (i = 100; i--;)
        y[i] = 666;
    memcpy(x, y, sizeof(int) * 1000);
    zprintf("x[0] = %x\n", x[0]);
    return 0;
}

