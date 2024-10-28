#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <stdnoreturn.h>

noreturn void child_proc(char *cmd, const char *const *args, uint32_t arg_count, int stdout_pipe, int stderr_pipe) {
    // We allocate two extra slots, one for the program name (argv[0]), and
    // one for the trailing NULL
    char *execvp_args[arg_count+2];
    execvp_args[0] = cmd;
    memcpy(execvp_args+1, args, sizeof(*args) * arg_count);
    execvp_args[arg_count+1] = NULL;

    bool failed = false;
    failed |= dup2(stdout_pipe, 1) == -1;
    failed |= dup2(stderr_pipe, 2) == -1;
    if (failed) {
        fprintf(stderr, "Failed to pipe stdout and stderr\n");
        exit(1);
    }

    close(stdout_pipe);
    close(stderr_pipe);

    execvp(cmd, execvp_args);

    fprintf(stderr, "Failed to start subprocess %s\n", cmd);
    exit(1);
}

#define PIPE_READ_AMOUNT 128

extern inline uint64_t grow_cap(uint64_t cap) {
    return (cap * 3) / 2 + PIPE_READ_AMOUNT + 1;
}

struct captured_txt {
    uint64_t cap;
    uint64_t len;
    char *str;
};

void guarantee_captured_cap(struct captured_txt *captured) {
    if (captured->len + PIPE_READ_AMOUNT >= captured->cap) {
        uint64_t new_cap = grow_cap(captured->cap);
        captured->cap = new_cap;
        captured->str = realloc(captured->str, new_cap);
    }
}

// returns: should the fd be kept
bool handle_fd(struct pollfd *fd, struct captured_txt *captured) {
    short events = fd->revents;

    if (events & POLLERR) {
        fprintf(stderr, "Failed to await new stderr/stdout data\n");
        exit(1);
    }

    if (events & POLLIN) {
        guarantee_captured_cap(captured);

        int64_t bytes_read = (uint64_t)read(
            fd->fd, captured->str+captured->len, PIPE_READ_AMOUNT);
        if (bytes_read == 0) {
            return true;
        } else if (bytes_read == -1) {
            fprintf(stderr, "Failed to read stdout/stderr from subprocess pipe\n");
            exit(1);
        }

        captured->len += bytes_read;
        captured->str[captured->len] = '\0';
    }

    if (events & POLLHUP) {
        return false;
    }  else {
        fd->revents = 0;
        return true;
    }
}

struct ProcessResult {
    char *output;
    unsigned char status;
};

void parent_proc(pid_t child_pid, int stdout_pipe, int stderr_pipe, struct ProcessResult *result) {
    struct captured_txt captured;
    memset(&captured, 0, sizeof(captured));

    struct pollfd all_fds[2];
    short notable_events = POLLIN;
    all_fds[0] = (struct pollfd) {stdout_pipe, notable_events, 0};
    all_fds[1] = (struct pollfd) {stderr_pipe, notable_events, 0};

    struct pollfd *fds_left = all_fds;
    nfds_t num_fd_left = 2;

    while (num_fd_left) {
        if (poll(fds_left, num_fd_left, -1) == -1) {
            fprintf(stderr, "Failed to await stdout/stderr from subprocess pipe\n");
            exit(1);
        }

        for (nfds_t i = 0; i < num_fd_left; i++) {
            if (handle_fd(fds_left + i, &captured)) continue;

            nfds_t remaining_fd = 1 - i;
            // this is easier to read than fds_left += remaining_fd
            fds_left = &fds_left[remaining_fd];
            num_fd_left--;
            i--;
        }
    }

    int wait_status;
    waitpid(child_pid, &wait_status, 0);
    int child_status = !WIFEXITED(wait_status) || WEXITSTATUS(wait_status);

    char *output = captured.str;
    if (output == NULL) {
        output = calloc(1, 1);
    }

    result->output = output;
    result->status = (unsigned char)child_status;
}

struct ProcessResult run_command(char *cmd, const char *const *args, uint32_t arg_count) {
    bool failed = false;
    int stdout_pipe[2];
    failed |= pipe(stdout_pipe);
    int stderr_pipe[2];
    failed |= pipe(stderr_pipe);
    if (failed) {
        fprintf(stderr, "Failed to intialize stdout or stderr pipe\n");
        exit(1);
    }

    pid_t pid = fork();

    if (pid < 0) {
        fprintf(stderr, "Failed to fork subprocess\n");
        exit(1);
    } else if (pid == 0) { // this is the child process
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);

        child_proc(cmd, args, arg_count, stdout_pipe[1], stderr_pipe[1]);
    } else { // this is the parent process
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        struct ProcessResult result;
        parent_proc(pid, stdout_pipe[0], stderr_pipe[0], &result);
        return result;
    }
}
