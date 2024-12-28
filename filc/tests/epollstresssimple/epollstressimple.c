#include <stdfil.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define REPEAT 100000
#define NTHREADS 1

static void writeloop(int fd, char* data, size_t size)
{
    while (size) {
        ssize_t result = write(fd, data, size);
        zprintf("Wrote %ld bytes.\n", result);
        ZASSERT(result);
        if (result == -1) {
            ZASSERT(errno == EINTR);
            continue;
        }
        ZASSERT(result > 0);
        ZASSERT(result <= size);
        data += result;
        size -= result;
    }
}

static size_t readloop(int fd, char* data, size_t size)
{
    size_t bytes_read = 0;
    while (size) {
        ssize_t result = read(fd, data, size);
        zprintf("Read %ld bytes.\n", result);
        if (!result)
            return bytes_read;
        if (result == -1) {
            ZASSERT(errno == EINTR);
            continue;
        }
        ZASSERT(result > 0);
        ZASSERT(result <= size);
        data += result;
        size -= result;
        bytes_read += result;
    }
    return bytes_read;
}

static void epoll_loop(int epfd, struct epoll_event* ev)
{
    for (;;) {
        int result = epoll_wait(epfd, ev, 1, -1);
        if (result >= 0) {
            ZASSERT(result == 1);
            return;
        }
        ZASSERT(errno == EINTR);
    }
}

static void close_loop(int fd)
{
    for (;;) {
        int result = close(fd);
        if (result >= 0) {
            ZASSERT(!result);
            return;
        }
        ZASSERT(errno == EINTR);
    }
}

int main()
{
    unsigned count;
    for (count = REPEAT; count--;) {
        int fds[2];
        ZASSERT(!pipe(fds));
        ZASSERT(fds[0] > 2);
        ZASSERT(fds[1] > 2);
        ZASSERT(fds[0] != fds[1]);
        writeloop(fds[1], "witaj", strlen("witaj") + 1);

        int epfd = epoll_create1(0);
        ZASSERT(epfd > 2);
        ZASSERT(epfd != fds[0]);
        ZASSERT(epfd != fds[1]);

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.u32 = count;
        ZASSERT(!epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev));
        memset(&ev, 0, sizeof(ev));
        epoll_loop(epfd, &ev);
        ZASSERT(ev.events == EPOLLIN);
        ZASSERT(ev.data.u32 == count);

        int epfd2;
        for (;;) {
            epfd2 = dup(epfd);
            if (epfd2 >= 0)
                break;
            ZASSERT(errno == EINTR);
        }
        ZASSERT(epfd2 > 2);
        ZASSERT(epfd2 != epfd);
        ZASSERT(epfd2 != fds[0]);
        ZASSERT(epfd2 != fds[1]);
        memset(&ev, 0, sizeof(ev));
        epoll_loop(epfd2, &ev);
        ZASSERT(ev.events == EPOLLIN);
        ZASSERT(ev.data.u32 == count);

        char buf[100];
        ZASSERT(readloop(fds[0], buf, strlen("witaj") + 1) == strlen("witaj") + 1);
        ZASSERT(!strcmp(buf, "witaj"));
        close_loop(epfd);
        close_loop(epfd2);
        close_loop(fds[0]);
        close_loop(fds[1]);
    }

    return 0;
}
