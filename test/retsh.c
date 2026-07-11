#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

extern char **environ;

// retux's shell. deliberately minimal - see retux's plan doc for the
// scope this was built against: no pipes, no redirection, no job
// control, no real $PATH (hardcoded to /bin), no quoting beyond plain
// whitespace-separated tokens. built-ins: cd (shell-local only - no
// chdir() syscall exists, so this just tracks a prompt-display string
// and always execs absolute /bin/<cmd> paths) and exit.

#define MAX_LINE 256
#define MAX_ARGS 32

static char cwd_display[64] = "/";

static int split(char *line, char *argv[MAX_ARGS]) {
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') ++p;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static void run_external(char *argv[], int argc) {
    char path[80];
    snprintf(path, sizeof(path), "/bin/%s", argv[0]);

    long pid = fork();
    if (pid < 0) {
        fprintf(stderr, "retsh: fork failed\n");
        return;
    }
    if (pid == 0) {
        execve(path, argv, environ);
        fprintf(stderr, "retsh: %s: command not found\n", argv[0]);
        _exit(127);
    }

    (void) argc;
    int status = 0;
    waitpid(pid, &status, 0);
}

int main(void) {
    char line[MAX_LINE];
    char *argv[MAX_ARGS];

    fprintf(stderr, "retsh - retux's shell\n");

    while (1) {
        fprintf(stderr, "retsh:%s$ ", cwd_display);

        ssize_t n = read(0, line, sizeof(line) - 1);
        if (n <= 0) continue;
        line[n] = '\0';

        int argc = split(line, argv);
        if (argc == 0) continue;

        if (strcmp(argv[0], "exit") == 0) {
            break;
        }
        if (strcmp(argv[0], "cd") == 0) {
            // shell-local only - no real chdir() syscall. just updates
            // the displayed prompt path so `cd` at least LOOKS like it
            // did something; every command is still exec'd from /bin
            // regardless.
            if (argc > 1) {
                strncpy(cwd_display, argv[1], sizeof(cwd_display) - 1);
                cwd_display[sizeof(cwd_display) - 1] = '\0';
            } else {
                strcpy(cwd_display, "/");
            }
            continue;
        }

        run_external(argv, argc);
    }

    return 0;
}
