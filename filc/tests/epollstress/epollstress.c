#include <stdfil.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define REPEAT 100000
#define NTHREADS 10

static void* thread_main(void* arg)
{
    ZASSERT(!arg);
    unsigned count;
    for (count = REPEAT; count--;) {
        int fds[2];
        ZASSERT(!pipe(fds));
        ZASSERT(fds[0] > 2);
        ZASSERT(fds[1] > 2);
        ZASSERT(fds[0] != fds[1]);
        ZASSERT(write(fds[1], "witaj", strlen("witaj") + 1) == strlen("witaj") + 1);

        int epfd = epoll_create1(0);
        ZASSERT(epfd > 2);
        ZASSERT(epfd != fds[0]);
        ZASSERT(epfd != fds[1]);

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.u32 = count;
        ZASSERT(!epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev));
        memset(&ev, 0, sizeof(ev));
        ZASSERT(epoll_wait(epfd, &ev, 1, -1) == 1);
        ZASSERT(ev.events == EPOLLIN);
        ZASSERT(ev.data.u32 == count);

        int epfd2 = dup(epfd);
        ZASSERT(epfd2 > 2);
        ZASSERT(epfd2 != epfd);
        ZASSERT(epfd2 != fds[0]);
        ZASSERT(epfd2 != fds[1]);
        memset(&ev, 0, sizeof(ev));
        ZASSERT(epoll_wait(epfd2, &ev, 1, -1) == 1);
        ZASSERT(ev.events == EPOLLIN);
        ZASSERT(ev.data.u32 == count);

        char buf[100];
        ZASSERT(read(fds[0], buf, strlen("witaj") + 1) == strlen("witaj") + 1);
        ZASSERT(!strcmp(buf, "witaj"));
        ZASSERT(!close(epfd));
        ZASSERT(!close(epfd2));
        ZASSERT(!close(fds[0]));
        ZASSERT(!close(fds[1]));
    }
    return NULL;
}

int main()
{
    pthread_t* threads = malloc(sizeof(pthread_t) * NTHREADS);

    unsigned index;
    for (index = NTHREADS; index--;)
        ZASSERT(!pthread_create(threads + index, NULL, thread_main, NULL));

    for (index = NTHREADS; index--;)
        ZASSERT(!pthread_join(threads[index], NULL));

    return 0;
}
