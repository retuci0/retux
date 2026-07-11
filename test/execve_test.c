#include <stdio.h>
#include <unistd.h>

int main(void) {
    fprintf(stderr, "execve_test: pid=%d, about to exec /argv_echo\n", getpid());

    char *argv[] = {"/argv_echo", "hello", "world", 0};
    char *envp[] = {"PATH=/bin", "FOO=bar", 0};
    execve("/argv_echo", argv, envp);

    // only reached if execve failed
    perror("execve");
    fprintf(stderr, "execve_test: execve returned (should be unreachable)\n");
    return 1;
}
