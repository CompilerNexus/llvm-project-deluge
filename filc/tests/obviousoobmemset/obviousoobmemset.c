#include <string.h>
#include <stdfil.h>

int main()
{
    int x[100];
    memset(x, 42, sizeof(int) * 1000);
    zprintf("x[0] = %x\n", x[0]);
    return 0;
}

