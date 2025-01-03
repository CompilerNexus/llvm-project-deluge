#include <stdfil.h>
#include <stdlib.h>
#include <inttypes.h>

int main()
{
    size_t count;
    for (count = 100000; count--;) {
        int** ptrs = malloc(sizeof(int*) * 1000);
        int* object = malloc(sizeof(int) * 1000);
        zsetcap(ptrs, object, sizeof(int*) * 1000);
        size_t index;
        for (index = 1000; index--;)
            ZASSERT(!ptrs[index]);
        for (index = 1000; index--;)
            ((uintptr_t*)ptrs)[index] = (uintptr_t)(object + index);
        for (index = 1000; index--;)
            *ptrs[index] = index * 42;
        for (index = 1000; index--;)
            ZASSERT(object[index] == index * 42);
    }
    return 0;
}

