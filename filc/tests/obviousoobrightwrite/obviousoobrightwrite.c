#include <stdfil.h>

int main()
{
    int x[100];
    x[1000] = 666;
    zprintf("got here\n");
    zprintf("thingy = %d\n", x[1000]);
    return 0;
}

