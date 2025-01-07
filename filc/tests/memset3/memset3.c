#include <stdfil.h>

int main(void)
{
    char buf[4];
    __builtin_memset(buf, 'f', 4);
    ZASSERT(buf[0] == 'f');
    ZASSERT(buf[1] == 'f');
    ZASSERT(buf[2] == 'f');
    ZASSERT(buf[3] == 'f');
    return 0;
}

