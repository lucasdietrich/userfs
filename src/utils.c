/*
 * Copyright (c) 2025 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "userfs.h"

#include <stdio.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

void hexdump(const void *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if (i % 16 == 0) {
            printf("%08zx  ", i);
        }
        printf("%02x ", ((unsigned char *)data)[i]);
        if (i % 16 == 15 || i == size - 1) {
            for (size_t j = i - (i % 16); j <= i; j++) {
                if (j % 16 == 0) {
                    printf(" |");
                }
                if (j < size) {
                    unsigned char c = ((unsigned char *)data)[j];
                    printf("%c", (c >= 32 && c <= 126) ? c : '.');
                } else {
                    printf(" ");
                }
            }
            printf("|\n");
        }
    }
}

int create_directory(const char *dir)
{
    struct stat sb;

    if (stat(dir, &sb) == 0) {
        if (S_ISDIR(sb.st_mode)) {
            LOG("[ mkdir %s ] already exists, skipping\n", dir);
            return 0;
        } else {
            ERR("Path exists but is not a directory: %s\n", dir);
            return -1;
        }
    }

    if (errno != ENOENT) {
        ERR("Failed to check directory existence: %s\n", dir);
        perror("stat");
        return -1;
    }

    __mode_t mode = 0755; // rwxr-xr-x

    // Directory does not exist, create it
    LOG("[ mkdir %s ] mode: %o\n", dir, mode);
    if (mkdir(dir, mode) != 0) {
        ERR("Failed to create directory: %s\n", dir);
        perror("mkdir");
        return -1;
    }

    return 0;
}

static void command_display(const char *program, const char *argv[])
{
    if (!program || !argv)
        return;

    LOG("[ cmd ] %s", program);
    for (int i = 1; argv[i]; i++) {
        const char *has_space = strchr(argv[i], ' ');
        LOG(has_space ? " '%s'" : " %s", argv[i]);
    }
    LOG("\n");
}

int command_run(char *buf, size_t *buflen, const char *program, const char *argv[])
{
    command_display(program, argv);

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

        execvp(program, (char *const *)argv);
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

        int status;
        ret = waitpid(pid, &status, 0);
        if (ret < 0) {
            perror("waitpid");
        } else if (WIFEXITED(status)) {
            ret = WEXITSTATUS(status); // ret now holds the exit code of the child
        } else {
            ret = -1; // Abnormal termination
        }
    }

cleanup:
    if (pipefd[0] != -1)
        close(pipefd[0]);
    if (pipefd[1] != -1)
        close(pipefd[1]);

    // TODO fix returned value if command failed
    return ret;
}
