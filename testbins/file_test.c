#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(void) {
    int fd = open("/greeting.txt", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open failed\n");
        return 1;
    }

    char buf[128] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    fprintf(stderr, "read %ld bytes: %s", (long)n, buf);
    return 0;
}
