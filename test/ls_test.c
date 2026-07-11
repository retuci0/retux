#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>

struct linux_dirent64 {
    unsigned long d_ino;
    long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

int main(void) {
    int fd = open("/proc", O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        fprintf(stderr, "open(/proc) failed\n");
        return 1;
    }

    char buf[512];
    for (;;) {
        long n = syscall(SYS_getdents64, fd, buf, sizeof(buf));
        if (n < 0) {
            fprintf(stderr, "getdents64 failed: %ld\n", n);
            return 1;
        }
        if (n == 0) break;

        long pos = 0;
        while (pos < n) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + pos);
            fprintf(stderr, "  ino=%lu name=%s\n", d->d_ino, d->d_name);
            pos += d->d_reclen;
        }
    }

    close(fd);
    fprintf(stderr, "ls_test: done\n");
    return 0;
}
