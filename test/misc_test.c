#include <stdio.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <time.h>

int main(void) {
    struct utsname u;
    uname(&u);
    fprintf(stderr, "uname: %s %s %s\n", u.sysname, u.nodename, u.machine);

    fprintf(stderr, "getpid: %d\n", getpid());

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "clock_gettime: %lds %ldns\n", (long)ts.tv_sec, (long)ts.tv_nsec);

    return 0;
}
