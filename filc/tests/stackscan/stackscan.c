#include <stdfil.h>
#include <stdbool.h>

static bool callback(zstack_frame_description description,
                     void* arg)
{
    ZASSERT(arg == (void*)666);
    ZASSERT(!description.can_catch);
    ZASSERT(!description.personality_function);
    ZASSERT(!description.eh_data);
    zprintf("%s,%s,%u,%u,%s;",
            description.function_name, description.filename, description.line, description.column,
            description.can_throw ? "yes" : "no");
    return true;
}

static void foo(void)
{
    zstack_scan(callback, (void*)666);
}

static void bar(void)
{
    foo();
}

int main()
{
    bar();
    zprintf("\n");
    return 0;
}

