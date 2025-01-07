#include <stdfil.h>

int main()
{
    int x[100];
    x[-1] = 666;
    zprintf("got here\n");
    zprintf("thingy = %d\n", x[-1]);
    return 0;
}

