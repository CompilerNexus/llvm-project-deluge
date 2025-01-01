#include <stdfil.h>

static __attribute__((__noinline__)) void foo(void)
{
    void* address = __builtin_return_address(0);
    zprintf("foo's address = %p\n", address);
    ZASSERT(address);
    ZASSERT(!zhasvalidcap(address));
}

int main()
{
    void* address = __builtin_return_address(0);
    zprintf("main's address = %p\n", address);
    ZASSERT(address);
    ZASSERT(!zhasvalidcap(address));
    foo();
    zprintf("Got here.\n");
    zprintf("Should not get here = %d\n", (int)*(char*)address);
    return 0;
}
