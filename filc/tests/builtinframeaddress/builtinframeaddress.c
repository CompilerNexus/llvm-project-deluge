#include <stdfil.h>

static __attribute__((__noinline__)) void foo(void* parent_address)
{
    void* address = __builtin_frame_address(0);
    zprintf("foo's address = %p\n", address);
    ZASSERT(address);
    ZASSERT(!zhasvalidcap(address));
    ZASSERT(address < parent_address);
}

int main()
{
    void* address = __builtin_frame_address(0);
    zprintf("main's address = %p\n", address);
    ZASSERT(address);
    ZASSERT(!zhasvalidcap(address));
    foo(address);
    zprintf("Got here.\n");
    zprintf("Should not get here = %d\n", (int)*(char*)address);
    return 0;
}
