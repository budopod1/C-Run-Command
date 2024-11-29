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

#define REQUIRE_FULL_RUNCOMMAND_INTERFACE
#include "runcommand.h"

typedef struct ProcessResult EPSLProcessResult;

struct CapturedText {
    uint64_t cap;
    uint64_t len;
    char *str;
};

struct CaptureSettings {
    unsigned int keep_stdout : 1;
    unsigned int keep_stderr : 1;
    unsigned int merge_stderr: 1;
};

noreturn static void child_proc(char *cmd, char **args, uint32_t arg_count, int stdout_pipe, int stderr_pipe) {
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

static inline uint64_t grow_cap(uint64_t cap) {
    return (cap / 2) * 3 + PIPE_READ_AMOUNT + 1;
}

static void guarantee_text_cap(struct CapturedText *txt) {
    if (txt->len + PIPE_READ_AMOUNT >= txt->cap) {
        uint64_t new_cap = grow_cap(txt->cap);
        txt->cap = new_cap;
        txt->str = realloc(txt->str, new_cap);
    }
}

static void handle_stream(struct pollfd *fd, struct CapturedText *txt) {
    short events = fd->revents;

    if (events & POLLERR) {
        fprintf(stderr, "Failed to await new stderr/stdout data\n");
        exit(1);
    }

    if (!(events & POLLIN)) return;

    while (true) {
        guarantee_text_cap(txt);

        int64_t bytes_read = (uint64_t)read(
            fd->fd, txt->str+txt->len, PIPE_READ_AMOUNT);
        if (bytes_read == 0) {
            break;
        } else if (bytes_read < 0) {
            fprintf(stderr, "Failed to read stdout/stderr from subprocess pipe\n");
            exit(1);
        }

        txt->len += bytes_read;
    }

    txt->str[txt->len] = '\0';
}

static bool should_close_fd(struct pollfd *fd) {
    if (fd->revents & POLLHUP) {
        return true;
    }  else {
        fd->revents = 0;
        return false;
    }
}

char *captured_text_to_str(struct CapturedText *txt) {
    if (txt->len == 0) {
        return calloc(1, 1);
    } else {
        return txt->str;
    }
}

static void parent_proc(pid_t child_pid, struct CaptureSettings settings, int stdout_pipe, int stderr_pipe, struct CRCProcessResult *result) {
    struct CapturedText out = {0, 0, NULL};
    struct CapturedText err = {0, 0, NULL};
    
    short notable_events = POLLIN;
    struct pollfd fds[2];
    struct CapturedText *txts[2];
    nfds_t fd_count = 0;
    
    if (settings.keep_stdout) {
        fds[fd_count] = (struct pollfd) {stdout_pipe, notable_events, 0};
        txts[fd_count] = &out;
        fd_count++;
    }
    
    if (settings.keep_stderr) {
        fds[fd_count] = (struct pollfd) {stderr_pipe, notable_events, 0};
        txts[fd_count] = settings.merge_stderr ? &out : &err;
        fd_count++;
    }

    while (fd_count) {
        if (poll(fds, fd_count, -1) == -1) {
            fprintf(stderr, "Subprocess connection unexpectedly failed\n");
            exit(1);
        }

        for (nfds_t i = 0; i < fd_count; i++) {
            struct pollfd *fd = fds + i;
            struct CapturedText *txt = txts[i];
            handle_stream(fd, txt);
            if (should_close_fd(fd)) {
                fd_count--;
                memcpy(fds+i, fds+i+1, sizeof(*fds)*(fd_count-i));
                memcpy(txts+i, txts+i+1, sizeof(*txts)*(fd_count-i));
                i--;
            }
        }
    }

    int wait_status;
    waitpid(child_pid, &wait_status, 0);
    int child_status = !WIFEXITED(wait_status) || WEXITSTATUS(wait_status);

    result->out = captured_text_to_str(&out);
    result->err = captured_text_to_str(&err);
    result->status = (unsigned char)child_status;
}

struct CRCProcessResult CRC_run_command(char *cmd, char **args, uint32_t arg_count, uint32_t capture_mode) {
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
    } else if (pid == 0) {
        // this is the child process
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);

        child_proc(cmd, args, arg_count, stdout_pipe[1], stderr_pipe[1]);
    } else {
        // this is the parent process
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        struct CaptureSettings settings = {
            capture_mode & 1, (capture_mode & 2) != 0, (capture_mode & 4) != 0
        };

        struct CRCProcessResult result;
        parent_proc(pid, settings, stdout_pipe[0], stderr_pipe[0], &result);
        return result;
    }
}

static void null_terminate_epsl_string(struct ARRAY_char *str) {
    if (str->capacity <= str->length) {
        uint64_t new_cap = str->capacity + 1;
        str->content = realloc(str->content, new_cap);
        str->capacity = new_cap;
    }
    
    str->content[str->length] = '\0';
}

static struct ARRAY_char *wrap_c_str_to_epsl_str(uint64_t ref_counter, char *c_str) {
    uint64_t str_len = strlen(c_str);

    struct ARRAY_char *epsl_str = malloc(sizeof(*epsl_str));
    epsl_str->ref_counter = ref_counter;
    epsl_str->capacity = str_len + 1;
    epsl_str->length = str_len;
    epsl_str->content = (unsigned char*)c_str;

    return epsl_str;
}

EPSLProcessResult *CRC_epsl_run_command(struct ARRAY_char *cmd, struct ARRAY_ARRAY_char *args, uint32_t capture_mode) {
    null_terminate_epsl_string(cmd);
    
    char *args_buffer[args->length];
    for (uint64_t i = 0; i < args->length; i++) {
        struct ARRAY_char *arg = args->content[i];
        null_terminate_epsl_string(arg);
        args_buffer[i] = (char*)arg->content;
    }
    
    struct CRCProcessResult result = CRC_run_command(
        (char*)cmd->content, args_buffer, args->length, capture_mode);
    
    EPSLProcessResult *epsl_result = malloc(sizeof(*epsl_result));
    epsl_result->ref_counter = 0;
    epsl_result->out = wrap_c_str_to_epsl_str(1, result.out);
    epsl_result->err = wrap_c_str_to_epsl_str(1, result.err);
    epsl_result->status = result.status;

    return epsl_result;
}
