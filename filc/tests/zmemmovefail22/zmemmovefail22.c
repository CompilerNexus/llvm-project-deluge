#include <stdfil.h>
#include <inttypes.h>
#include <string.h>

int main()
{
    char* dst[4];
    int64_t src[4];

    unsigned i = 0;
    for (i = 4; i--;)
        dst[i] = "hello";

    zmemmove(dst, src, 1);

    for (i = 1; i < 4; ++i)
        ZASSERT(!strcmp(dst[i], "hello"));
    ZASSERT(!zhasvalidcap(dst[0]));

    return 0;
}
