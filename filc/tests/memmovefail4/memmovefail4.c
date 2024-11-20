#include <stdlib.h>
#include <string.h>
#include <stdfil.h>

struct foo {
    int x;
    int y;
    char* str;
    int z;
    int w;
};

int main()
{
    struct foo a;

    a.x = 1;
    a.y = 2;
    a.str = "hello";
    a.z = 3;
    a.w = 4;

    zmemmove(&a.y, &a, sizeof(struct foo) - __builtin_offsetof(struct foo, y));

    struct foo* b = (struct foo*)&a.y;

    ZASSERT(b->x == 1);
    ZASSERT(b->y == 2);
    ZASSERT(b->z == 3);
    ZASSERT(a.x == 1);
    ZASSERT(!zhasvalidcap(a.str));

    return 0;
}

