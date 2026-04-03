#include "shell_utils.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int shell_quote(const char *input, char *out, size_t out_size) {
    static const char single_quote_escape[] = "'\"'\"'";
    size_t idx = 0;

    if (input == NULL || out == NULL || out_size < 3) {
        return -1;
    }

    out[idx++] = '\'';
    while (*input != '\0') {
        if (*input == '\'') {
            size_t esc_len = strlen(single_quote_escape);
            if (idx + esc_len + 2 >= out_size) {
                return -1;
            }
            memcpy(out + idx, single_quote_escape, esc_len);
            idx += esc_len;
        } else {
            if (idx + 2 >= out_size) {
                return -1;
            }
            out[idx++] = *input;
        }
        input++;
    }

    out[idx++] = '\'';
    out[idx] = '\0';
    return 0;
}

int ensure_directory(const char *path) {
    char buffer[PATH_MAX];
    size_t len;
    char *cursor;

    if (path == NULL || *path == '\0') {
        return -1;
    }

    len = strlen(path);
    if (len >= sizeof(buffer)) {
        return -1;
    }

    memcpy(buffer, path, len + 1);

    for (cursor = buffer + 1; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *cursor = '/';
        }
    }

    if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

int remove_tree(const char *path) {
    struct stat st;

    if (path == NULL || *path == '\0') {
        return -1;
    }

    if (lstat(path, &st) != 0) {
        return (errno == ENOENT) ? 0 : -1;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;
        if (dir == NULL) {
            return -1;
        }

        while ((entry = readdir(dir)) != NULL) {
            char child[PATH_MAX];
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) < 0) {
                closedir(dir);
                return -1;
            }
            if (remove_tree(child) != 0) {
                closedir(dir);
                return -1;
            }
        }

        closedir(dir);
        return rmdir(path);
    }

    return unlink(path);
}

int run_command_capture(const char *command, const char *workdir, int timeout_sec, StringBuilder *output, int *exit_code) {
    int pipefd[2];
    pid_t child;
    time_t start_time;
    int status = 0;
    int child_done = 0;

    if (command == NULL) {
        return -1;
    }

    if (pipe(pipefd) != 0) {
        return -1;
    }

    child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (child == 0) {
        if (workdir != NULL && *workdir != '\0' && chdir(workdir) != 0) {
            dprintf(pipefd[1], "chdir failed: %s\n", strerror(errno));
            _exit(127);
        }

        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-lc", command, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    start_time = time(NULL);

    while (!child_done) {
        fd_set readfds;
        struct timeval timeout;
        char buffer[512];
        ssize_t nread;
        pid_t waited;

        FD_ZERO(&readfds);
        FD_SET(pipefd[0], &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

        if (select(pipefd[0] + 1, &readfds, NULL, NULL, &timeout) > 0 && FD_ISSET(pipefd[0], &readfds)) {
            while ((nread = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
                if (output != NULL) {
                    sb_append_n(output, buffer, (size_t)nread);
                }
            }
        }

        waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            child_done = 1;
        }

        if (!child_done && timeout_sec > 0 && difftime(time(NULL), start_time) >= timeout_sec) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            if (output != NULL) {
                sb_append(output, "\ncommand timed out\n");
            }
            close(pipefd[0]);
            if (exit_code != NULL) {
                *exit_code = -1;
            }
            return -1;
        }
    }

    for (;;) {
        char buffer[512];
        ssize_t nread = read(pipefd[0], buffer, sizeof(buffer));
        if (nread <= 0) {
            break;
        }
        if (output != NULL) {
            sb_append_n(output, buffer, (size_t)nread);
        }
    }

    close(pipefd[0]);

    if (exit_code != NULL) {
        if (WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *exit_code = 128 + WTERMSIG(status);
        } else {
            *exit_code = -1;
        }
    }

    return 0;
}

int start_background_command(const char *command, const char *workdir, int confirm_sec, StringBuilder *output, int *started_in_background, int *exit_code, int *child_pid) {
    int pipefd[2];
    pid_t supervisor;
    int status = 0;
    char buffer[256];
    ssize_t nread;
    size_t length = 0;

    if (command == NULL) {
        return -1;
    }

    if (started_in_background != NULL) {
        *started_in_background = 0;
    }
    if (child_pid != NULL) {
        *child_pid = -1;
    }

    if (pipe(pipefd) != 0) {
        return -1;
    }

    supervisor = fork();
    if (supervisor < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (supervisor == 0) {
        pid_t worker;
        int worker_status = 0;
        int loops;

        close(pipefd[0]);

        if (workdir != NULL && *workdir != '\0' && chdir(workdir) != 0) {
            dprintf(pipefd[1], "CHDIR:%s\n", strerror(errno));
            _exit(1);
        }

        worker = fork();
        if (worker < 0) {
            dprintf(pipefd[1], "FORK:%s\n", strerror(errno));
            _exit(1);
        }

        if (worker == 0) {
            int log_fd;
            setsid();
            log_fd = open("/tmp/iot-ide-start.log", O_CREAT | O_WRONLY | O_APPEND, 0644);
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                if (log_fd > STDERR_FILENO) {
                    close(log_fd);
                }
            }
            execl("/bin/sh", "sh", "-lc", command, (char *)NULL);
            _exit(127);
        }

        for (loops = 0; loops < confirm_sec * 10; ++loops) {
            pid_t waited = waitpid(worker, &worker_status, WNOHANG);
            if (waited == worker) {
                int code = WIFEXITED(worker_status) ? WEXITSTATUS(worker_status) : (128 + WTERMSIG(worker_status));
                dprintf(pipefd[1], "EXIT:%d\n", code);
                _exit(code == 0 ? 0 : 1);
            }
            usleep(100000);
        }

        dprintf(pipefd[1], "STARTED:%d\n", worker);
        _exit(0);
    }

    close(pipefd[1]);
    while ((nread = read(pipefd[0], buffer + length, sizeof(buffer) - 1 - length)) > 0) {
        length += (size_t)nread;
        if (length >= sizeof(buffer) - 1) {
            break;
        }
    }
    buffer[length] = '\0';
    close(pipefd[0]);
    waitpid(supervisor, &status, 0);

    if (output != NULL && buffer[0] != '\0') {
        sb_append(output, buffer);
    }

    if (strncmp(buffer, "STARTED:", 8) == 0) {
        int pid = atoi(buffer + 8);
        if (started_in_background != NULL) {
            *started_in_background = 1;
        }
        if (child_pid != NULL) {
            *child_pid = pid;
        }
        if (exit_code != NULL) {
            *exit_code = 0;
        }
        return 0;
    }

    if (strncmp(buffer, "EXIT:", 5) == 0) {
        int code = atoi(buffer + 5);
        if (exit_code != NULL) {
            *exit_code = code;
        }
        return (code == 0) ? 0 : -1;
    }

    if (exit_code != NULL) {
        *exit_code = -1;
    }
    return -1;
}
