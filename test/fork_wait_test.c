#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    fprintf(stderr, "parent: pid=%d, forking\n", getpid());

    long pid = fork();
    if (pid == 0) {
        fprintf(stderr, "child: pid=%d, exiting with code 7\n", getpid());
        return 7;
    }

    fprintf(stderr, "parent: forked child pid=%ld, waiting\n", pid);
    int status = 0;
    long waited = waitpid(pid, &status, 0);
    fprintf(stderr, "parent: waitpid returned %ld, WIFEXITED=%d WEXITSTATUS=%d\n",
            waited, WIFEXITED(status), WEXITSTATUS(status));

    fprintf(stderr, "parent: forking a second child\n");
    pid = fork();
    if (pid == 0) {
        fprintf(stderr, "child2: pid=%d, exiting with code 99\n", getpid());
        return 99;
    }
    waited = waitpid(pid, &status, 0);
    fprintf(stderr, "parent: waitpid #2 returned %ld, WEXITSTATUS=%d\n",
            waited, WEXITSTATUS(status));

    fprintf(stderr, "parent: done\n");
    return 0;
}
