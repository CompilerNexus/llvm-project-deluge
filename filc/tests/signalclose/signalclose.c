#include <unistd.h>
#include <signal.h>
#include <stdfil.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <pizlonated_runtime.h>

static int fd;
static bool did_run;

static void handler(int signo)
{
    int result = close(fd);
    if (result) {
        ZASSERT(result == -1);
        ZASSERT(errno == EBADF);
    }
    did_run = true;
}

int main()
{
    signal(SIGALRM, handler);
    unsigned i;
    for (i = 100; i--;) {
        fd = open("filc/tests/signalclose/signalclose.c", O_RDONLY);
        did_run = false;
        ZASSERT(fd > 2);
        ualarm(1, 0);
        while (!did_run) {
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

