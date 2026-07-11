#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    // identity: getpid() + uname() round-trip through the syscall table
    struct utsname u;
    uname(&u);
    fprintf(stderr, "pid %d on %s %s (%s)\n", getpid(), u.sysname, u.release, u.machine);

    // heap: malloc forces brk/mmap growth, not just a static buffer
    char *buf = malloc(256);
    snprintf(buf, 256, "heap buffer at %p, backed by brk/mmap", (void *) buf);
    fprintf(stderr, "%s\n", buf);
    free(buf);

    // filesystem: read a file back out of the mounted initrd through the VFS
    int fd = open("/greeting.txt", O_RDONLY);
    if (fd >= 0) {
        char line[128] = {0};
        ssize_t n = read(fd, line, sizeof(line) - 1);
        close(fd);
        fprintf(stderr, "read %ld bytes from /greeting.txt: %s", (long) n, line);
    } else {
        fprintf(stderr, "open(/greeting.txt) failed - is it in initrdroot/?\n");
    }

    // time: clock_gettime
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "clock_gettime: %lds %ldns since boot\n", (long) ts.tv_sec, (long) ts.tv_nsec);

    // deliberately hit a syscall retux doesn't implement yet, to show the
    // graceful fallback (logs the number, returns -ENOSYS) instead of a
    // page fault or triple fault
    char cwd[64];
    long rc = syscall(SYS_getcwd, cwd, sizeof(cwd));
    fprintf(stderr, "getcwd() returned %ld, errno %d (expect -1/ENOSYS - not implemented yet)\n",
            rc, errno);

    fprintf(stderr, "exiting cleanly with code 42\n");
    return 42;
}
