#include <unistd.h>
#include <signal.h>
#include <stdfil.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <pizlonated_runtime.h>
#include <setjmp.h>

static int fd;
static sigjmp_buf jb;

static void handler(int signo)
{
    int result = close(fd);
    if (result) {
        ZASSERT(result == -1);
        ZASSERT(errno == EBADF);
    }
    siglongjmp(jb, 1);
}

int main()
{
    signal(SIGALRM, handler);
    volatile unsigned i;
    for (i = 100; i--;) {
        if (sigsetjmp(jb, 1))
            continue;
        fd = open("filc/tests/signalclose/signalclose.c", O_RDONLY);
        ZASSERT(fd > 2);
        ualarm(1, 0);
        for (;;) {
            int result = close(fd);
            if (result) {
                ZASSERT(result == -1);
                ZASSERT(errno == EBADF);
            }
        }
    }
    ZASSERT(!zget_signal_deferral_depth());
    printf("Znakomicie\n");
    return 0;
}

