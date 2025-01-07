#include <stdfil.h>

int main(void)
{
    char buf[4];
    __builtin_memset(buf, 'f', 4);
    zprintf("buf = %c%c%c%c\n", buf[0], buf[1], buf[2], buf[3]);
    return 0;
}

