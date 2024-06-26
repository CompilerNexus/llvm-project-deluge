#include <unistd.h>
#include <stdio.h>
#include <pizlonated_common_syscalls.h>
#include <pizlonated_runtime.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <grp.h>
#include <stdbool.h>

int main(int argc, char** argv)
{
    unsigned zid;
    unsigned id;
    int res;
    
    zid = zsys_getuid();
    id = getuid();
    ZASSERT((int)zid > 0);
    ZASSERT(zid == id);
    
    zid = zsys_geteuid();
    id = geteuid();
    ZASSERT((int)zid > 0);
    ZASSERT(zid == id);
    
    zid = zsys_getgid();
    id = getgid();
    ZASSERT((int)zid > 0);
    ZASSERT(zid == id);
    
    zid = zsys_getpid();
    id = getpid();
    ZASSERT((int)zid > 0);
    ZASSERT(zid == id);

    struct timespec ts;
    res = clock_gettime(CLOCK_REALTIME, &ts);
    ZASSERT(!res);
    ZASSERT(ts.tv_sec);
    ZASSERT(ts.tv_nsec < 1000000000llu);

    struct timeval tv;
    res = gettimeofday(&tv, NULL);
    ZASSERT(!res);
    ZASSERT(tv.tv_sec);
    ZASSERT(tv.tv_usec < 1000000llu);

    struct stat st;
    zprintf("size = %zu\n", sizeof(st));
    zprintf("offset of nlink = %zu\n", __builtin_offsetof(struct stat, st_nlink));
    zprintf("sizeof nlink = %zu\n", sizeof(st.st_nlink));
    zprintf("sizeof(nlink_t) = %zu\n", sizeof(nlink_t));
    res = stat("filc/tests/miscsyscall/testfile.txt", &st);
    ZASSERT(!res);
    ZASSERT(st.st_mode == (0644 | S_IFREG));
    ZASSERT(st.st_nlink == 1);
    ZASSERT(st.st_uid);
    ZASSERT(st.st_gid);
    ZASSERT(st.st_size == 86);

    ZASSERT(zthread_self());
    ZASSERT(zthread_self() == zthread_self());
    ZASSERT((void*)pthread_self() == zthread_self());

    ZASSERT(open("this/is/totally/not/going/to/exist", 666, 42) < 0);
    printf("the error was: %s\n", strerror(errno));

    int fd = open("filc/tests/miscsyscall/testfile.txt", O_RDONLY);
    ZASSERT(fd > 2);
    struct stat st2;
    res = fstat(fd, &st2);
    ZASSERT(!res);
    ZASSERT(st2.st_mode == (0644 | S_IFREG));
    ZASSERT(st2.st_nlink == 1);
    ZASSERT(st2.st_uid == st.st_uid);
    ZASSERT(st2.st_gid == st.st_gid);
    ZASSERT(st2.st_size == 86);

    struct passwd* passwd = getpwuid(getuid());
    ZASSERT(passwd);
    ZASSERT(strlen(passwd->pw_name));
    ZASSERT(strlen(passwd->pw_passwd));
    ZASSERT(passwd->pw_uid == getuid());
    ZASSERT(passwd->pw_gid == getgid());
    ZASSERT(strlen(passwd->pw_gecos));
    ZASSERT(strlen(passwd->pw_dir));
    ZASSERT(strlen(passwd->pw_shell));

    char* name = strdup(passwd->pw_name);
    char* passwdd = strdup(passwd->pw_passwd);
    char* gecos = strdup(passwd->pw_gecos);
    char* dir = strdup(passwd->pw_dir);
    char* shell = strdup(passwd->pw_shell);
    
    struct passwd* passwd2 = getpwuid(getuid());
    ZASSERT(passwd2);
    ZASSERT(passwd2 != passwd);
    ZASSERT(!strcmp(passwd2->pw_name, name));
    ZASSERT(!strcmp(passwd2->pw_passwd, passwdd));
    ZASSERT(passwd2->pw_uid == getuid());
    ZASSERT(passwd2->pw_gid == getgid());
    ZASSERT(!strcmp(passwd2->pw_gecos, gecos));
    ZASSERT(!strcmp(passwd2->pw_dir, dir));
    ZASSERT(!strcmp(passwd2->pw_shell, shell));

    passwd = getpwnam(name);
    ZASSERT(passwd);
    ZASSERT(passwd != passwd2);
    ZASSERT(!strcmp(passwd->pw_name, name));
    ZASSERT(!strcmp(passwd->pw_passwd, passwdd));
    ZASSERT(passwd->pw_uid == getuid());
    ZASSERT(passwd->pw_gid == getgid());
    ZASSERT(!strcmp(passwd->pw_gecos, gecos));
    ZASSERT(!strcmp(passwd->pw_dir, dir));
    ZASSERT(!strcmp(passwd->pw_shell, shell));

    ZASSERT(signal(SIGPIPE, SIG_IGN) == SIG_DFL);

    struct sigaction act;
    struct sigaction oact;
    act.sa_handler = SIG_DFL;
    sigfillset(&act.sa_mask);
    act.sa_flags = SA_NODEFER;
    ZASSERT(sigaction(SIGPIPE, &act, &oact) == 0);
    ZASSERT(oact.sa_handler == SIG_IGN);
    ZASSERT(oact.sa_flags == SA_RESTART);
    ZASSERT(!sigismember(&oact.sa_mask, SIGPIPE));
    ZASSERT(!sigismember(&oact.sa_mask, SIGTERM));

    ZASSERT(sigaction(SIGPIPE, NULL, &oact) == 0);
    ZASSERT(oact.sa_handler == SIG_DFL);
    ZASSERT(oact.sa_flags == SA_NODEFER);
    ZASSERT(sigismember(&oact.sa_mask, SIGPIPE));
    ZASSERT(sigismember(&oact.sa_mask, SIGTERM));

    act.sa_handler = SIG_IGN;
    ZASSERT(sigaction(SIGPIPE, &act, NULL) == 0);

    struct rlimit rlim;
    ZASSERT(!getrlimit(RLIMIT_NPROC, &rlim));
    ZASSERT(!getrlimit(RLIMIT_CPU, &rlim));

    ZASSERT(umask(0644));
    ZASSERT(umask(0644) == 0644);

    struct utsname utsname;
    ZASSERT(!uname(&utsname));
    ZASSERT(strlen(utsname.sysname));
    ZASSERT(strlen(utsname.nodename));
    ZASSERT(strlen(utsname.release));
    ZASSERT(strlen(utsname.version));
    ZASSERT(strlen(utsname.machine));
    strlen(utsname.__domainname); // Run for effect; the string may be empty!

    struct itimerval timerval;
    memset(&timerval, 42, sizeof(struct itimerval));
    ZASSERT(!getitimer(ITIMER_REAL, &timerval));
    ZASSERT(!timerval.it_interval.tv_sec);
    ZASSERT(!timerval.it_interval.tv_usec);
    ZASSERT(!timerval.it_value.tv_sec);
    ZASSERT(!timerval.it_value.tv_usec);

    ZASSERT(getppid());
    ZASSERT(getppid() != getpid());

    struct group* group = getgrnam("tty");
    ZASSERT(group);
    ZASSERT(!strcmp(group->gr_name, "tty"));
    ZASSERT(group->gr_gid);

    char buf[256];
    ZASSERT(!getentropy(buf, 256));
    bool all_zero = true;
    size_t index;
    for (index = 256; index--;) {
        if (buf[index])
            all_zero = false;
    }
    ZASSERT(!all_zero);

    unlink("filc/tests/miscsyscall/readonly.txt");
    unlink("filc/tests/miscsyscall/writeonly.txt");
    unlink("filc/tests/miscsyscall/execonly.txt");
    fd = open("filc/tests/miscsyscall/readonly.txt", O_CREAT | O_WRONLY, 0600);
    ZASSERT(fd > 2);
    close(fd);
    ZASSERT(!chmod("filc/tests/miscsyscall/readonly.txt", 0400));
    fd = open("filc/tests/miscsyscall/writeonly.txt", O_CREAT | O_WRONLY, 0600);
    ZASSERT(fd > 2);
    close(fd);
    ZASSERT(!chmod("filc/tests/miscsyscall/writeonly.txt", 0200));
    fd = open("filc/tests/miscsyscall/execonly.txt", O_CREAT | O_WRONLY, 0600);
    ZASSERT(fd > 2);
    close(fd);
    ZASSERT(!chmod("filc/tests/miscsyscall/execonly.txt", 0100));
    ZASSERT(!access("filc/tests/miscsyscall/readonly.txt", F_OK));
    ZASSERT(!access("filc/tests/miscsyscall/writeonly.txt", F_OK));
    ZASSERT(!access("filc/tests/miscsyscall/execonly.txt", F_OK));
    ZASSERT(!access("filc/tests/miscsyscall/readonly.txt", R_OK));
    ZASSERT(access("filc/tests/miscsyscall/readonly.txt", W_OK));
    ZASSERT(errno == EACCES);
    ZASSERT(access("filc/tests/miscsyscall/readonly.txt", X_OK));
    ZASSERT(errno == EACCES);
    ZASSERT(access("filc/tests/miscsyscall/writeonly.txt", R_OK));
    ZASSERT(errno == EACCES);
    ZASSERT(!access("filc/tests/miscsyscall/writeonly.txt", W_OK));
    ZASSERT(access("filc/tests/miscsyscall/writeonly.txt", X_OK));
    ZASSERT(errno == EACCES);
    ZASSERT(access("filc/tests/miscsyscall/execonly.txt", R_OK));
    ZASSERT(errno == EACCES);
    ZASSERT(access("filc/tests/miscsyscall/execonly.txt", W_OK));
    ZASSERT(errno == EACCES);
    ZASSERT(!access("filc/tests/miscsyscall/execonly.txt", X_OK));

    zprintf("No worries.\n");
    return 0;
}

