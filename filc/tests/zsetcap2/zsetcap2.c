#include <stdfil.h>
#include <stdlib.h>
#include <inttypes.h>

int main()
{
    int* ptrs[100];
    int* object = malloc(sizeof(int) * 100);
    zsetcap(ptrs, object, sizeof(int*) * 100);
    size_t index;
    for (index = 100; index--;)
        ZASSERT(!ptrs[index]);
    for (index = 100; index--;)
        ((uintptr_t*)ptrs)[index] = (uintptr_t)(object + index);
    for (index = 100; index--;)
        *ptrs[index] = index * 42;
    for (index = 100; index--;)
        ZASSERT(object[index] == index * 42);
    return 0;
}

