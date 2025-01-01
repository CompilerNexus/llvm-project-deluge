#include <stdfil.h>

int main()
{
    zprintf("Should not get here = %p\n", __builtin_frame_address(1));
    return 0;
}

