#include <stdfil.h>
#include <stdlib.h>
#include <inttypes.h>

int main()
{
    int* ptr;
    int* object = malloc(sizeof(int));
    zsetcap(&ptr, object, sizeof(int*));
    ZASSERT(!ptr);
    *(uintptr_t*)&ptr = (uintptr_t)object;
    *ptr = 42;
    ZASSERT(*object == 42);
    return 0;
}

