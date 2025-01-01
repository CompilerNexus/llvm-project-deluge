#include <stdfil.h>

int main()
{
    zprintf("Should not get here = %p\n", __builtin_return_address(1));
    return 0;
}

