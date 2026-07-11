#include <stdio.h>

extern char **environ;

int main(int argc, char **argv) {
    fprintf(stderr, "argv_echo: argc=%d\n", argc);
    for (int i = 0; i < argc; ++i) {
        fprintf(stderr, "  argv[%d] = %s\n", i, argv[i]);
    }
    int envc = 0;
    for (char **e = environ; *e; ++e) {
        fprintf(stderr, "  envp[%d] = %s\n", envc, *e);
        ++envc;
    }
    fprintf(stderr, "argv_echo: done\n");
    return 0;
}
