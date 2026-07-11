#include <stdio.h>
#include <unistd.h>

int main(void) {
    fprintf(stderr, "before fork, pid=%d\n", getpid());

    long pid = fork();
    if (pid == 0) {
        fprintf(stderr, "child: pid=%d, fork() returned=%ld\n", getpid(), pid);
    } else if (pid > 0) {
        fprintf(stderr, "parent: pid=%d, fork() returned child pid=%ld\n", getpid(), pid);
    } else {
        fprintf(stderr, "fork() failed\n");
        return 1;
    }

    fprintf(stderr, "exiting, pid=%d\n", getpid());
    return 0;
}
