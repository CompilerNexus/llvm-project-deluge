#include <stdfil.h>
#include <stdlib.h>
#include <inttypes.h>

int main()
{
    int* ptr;
    int* object = malloc(sizeof(int));
    *(uintptr_t*)&ptr = (uintptr_t)object;
    *ptr = 42;
    ZASSERT(*object == 42);
    return 0;
}

