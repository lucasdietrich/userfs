#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>

#include "userfs.h"
#include "utils.h"


int create_directory(const char *dir)
{
    struct stat sb;

    LOG("Creating directory: %s\n", dir);

    if (stat(dir, &sb) == 0) {
        if (S_ISDIR(sb.st_mode)) {
            LOG("Directory already exists: %s\n", dir);
            return 0;
        } else {
            fprintf(stderr, "Path exists but is not a directory: %s\n", dir);
            return -1;
        }
    }

    if (errno != ENOENT) {
        fprintf(stderr, "Failed to check directory existence: %s\n", dir);
        perror("stat");
        return -1;
    }

    // Directory does not exist, try to create it
    if (mkdir(dir, 0755) != 0) {
        fprintf(stderr, "Failed to create directory: %s\n", dir);
        perror("mkdir");
        return -1;
    }

    return 0;
}


void command_display(const char *program, char *const argv[])
{
    if (!program || !argv) return;

    printf("Running command: %s ", program);
    for (int i = 0; argv[i]; i++) {
        printf("%s ", argv[i]);
    }
    printf("\n");
}

int command_run(char *buf, size_t *buflen, const char *program, char *const argv[])
{
    int ret       = -1;
    int pipefd[2] = {-1, -1}; // [0] = read, [1] = write
    pid_t pid;
    bool capture_output = (buf && buflen);

    if ((!program || !argv) || (buf && !buflen) || (!buf && buflen) ||
        (capture_output && *buflen == 0)) {
        errno = EINVAL;
        return -1;
    }

    if (capture_output && pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        goto cleanup;
    } else if (pid == 0) {
        // Child
        if (capture_output) {
            close(pipefd[0]); // Close read end

            if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
                perror("dup2");
                _exit(EXIT_FAILURE);
            }

            close(pipefd[1]); // Not needed after dup2
        }

        execvp(program, argv);
        // If execvp returns, it failed
        perror("execvp");
        _exit(EXIT_FAILURE);
    } else {
        // Parent
        if (capture_output) {
            close(pipefd[1]); // Close write end

            ssize_t nread = read(pipefd[0], buf, *buflen);
            if (nread < 0) {
                perror("read");
                goto cleanup;
            }
            *buflen = (size_t)nread;
        }

        ret = waitpid(pid, NULL, 0);
        if (ret < 0) {
            perror("waitpid");
        }
    }

cleanup:
    if (pipefd[0] != -1) close(pipefd[0]);
    if (pipefd[1] != -1) close(pipefd[1]);

    // TODO fix returned value if command failed
    return ret;
}
