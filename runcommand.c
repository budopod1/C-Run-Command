#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#define REQUIRE_FULL_RUNCOMMAND_INTERFACE
#include "runcommand.h"

#define PIPE_READ_AMOUNT 128

struct CapturedText {
    uint64_t cap;
    uint64_t len;
    char *str;
};

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

static char *captured_text_to_str(struct CapturedText *txt) {
    if (txt->len == 0) {
        return calloc(1, 1);
    } else {
        return txt->str;
    }
}

static void verify_capture_mode(uint32_t capture_mode) {
    if (capture_mode & CAPTURE_MODE_MERGE_STDERR &&
        !(capture_mode & CAPTURE_MODE_KEEP_STDOUT
        && capture_mode & CAPTURE_MODE_KEEP_STDERR)) {
        fprintf(stderr, "Cannot merge stderr into stdout unless both are already captured\n");
        exit(1);
    }
}

#ifdef _MSC_VER

#include <windows.h>

static bool does_require_escaping(char *str) {
    if (*str == '\0') return true;
    while (true) {
        char chr = *(str++);
        if (chr == '\0') return false;
        if (!(('A' <= chr && chr <= 'Z')
            || ('a' <= chr && chr <= 'z')
            || ('0' <= chr && chr <= '9')
            || chr == '-' || chr == '_'
            || chr == '.' || chr == '\\')) {
            return true;
        }
    }
}

static uint32_t escaped_cmd_arg_len(char *arg) {
    uint32_t len = 2;
    while (true) {
        char chr = *(arg++);
        if (chr == '\0') return len;
        if (chr == '"') len++;
        len++;
    }
}

static void escape_cmd_arg(char *arg, char *dest, char **nxt_pos) {
    *dest = '"';
    while (true) {
        char chr = *(arg++);
        if (chr == '\0') break;
        if (chr == '"') *++dest = '"';
        *++dest = chr;
    }
    *++dest = '"';
    *++dest = '\0';
    if (nxt_pos != NULL) {
        *nxt_pos = dest;
    }
}

static void make_pipe_pair(PHANDLE par_handle, PHANDLE sub_handle) {
    SECURITY_ATTRIBUTES security_attributes;
    ZeroMemory(&security_attributes, sizeof(security_attributes));
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.lpSecurityDescriptor = NULL;
    security_attributes.bInheritHandle = true;

    if (!CreatePipe(par_handle, sub_handle, &security_attributes, 0)) {
        fprintf(stderr, "Failed to open pipe\n");
        exit(1);
    }

    if (!SetHandleInformation(*par_handle, HANDLE_FLAG_INHERIT, 0)) {
        fprintf(stderr, "Failed to set pipe permissions\n");
        exit(1);
    }
}

static void capture_pipe_out(HANDLE pipe, struct CapturedText *txt) {
    while (true) {
        guarantee_text_cap(txt);

        DWORD bytes_read;
        BOOL status = ReadFile(
            pipe, // handle to be read from
            txt->str + txt->len, // dest buffer
            PIPE_READ_AMOUNT, // max amount to be read
            &bytes_read, // set to actual amount read
            NULL // unused OVERLAPPED*
        );
        if (!status) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                CloseHandle(pipe);
                break;
            } else {
                fprintf(stderr, "Failed to read from pipe\n");
                exit(1);
            }
        }

        txt->len += bytes_read;
    }

    txt->str[txt->len] = '\0';
}

DLL_EXPORT
struct CRCProcessResult CRC_run_command(char *cmd, char **args, uint32_t arg_count, uint32_t capture_mode) {
    verify_capture_mode(capture_mode);

    bool should_escape_cmd = does_require_escaping(cmd);

    uint32_t unescaped_cmd_len = strlen(cmd);
    uint32_t cmd_str_len = should_escape_cmd ? escaped_cmd_arg_len(cmd) : unescaped_cmd_len;
    cmd_str_len += arg_count; // the spaces
    for (uint32_t i = 0; i < arg_count; i++) {
        cmd_str_len += escaped_cmd_arg_len(args[i]);
    }

    char cmd_str[cmd_str_len+1];
    char *cmd_str_p = cmd_str;

    if (should_escape_cmd) {
        escape_cmd_arg(cmd, cmd_str_p, &cmd_str_p);
    } else {
        memcpy(cmd_str_p, cmd, unescaped_cmd_len);
        cmd_str_p += unescaped_cmd_len;
    }

    for (uint32_t i = 0; i < arg_count; i++) {
        *(cmd_str_p++) = ' ';
        escape_cmd_arg(args[i], cmd_str_p, &cmd_str_p);
    }

    int wide_cmd_str_size = MultiByteToWideChar(
        CP_UTF8, // source encoding
        MB_ERR_INVALID_CHARS, // flags
        cmd_str, // src str
        -1, // src len (-1 for NULL terminated)
        NULL, // dest buffer (ignored due to next param)
        0 // dest buffer size (0 indicated do not write, just calc size)
    );

    if (wide_cmd_str_size == 0) {
        fprintf(stderr, "Cannot re-encode command %s\n", cmd_str);
        exit(1);
    }

    wchar_t wide_cmd_str[wide_cmd_str_size];

    int status = MultiByteToWideChar(
        CP_UTF8, // source encoding
        MB_ERR_INVALID_CHARS, // flags
        cmd_str, // src str
        -1, // src len (-1 for NULL terminated)
        wide_cmd_str, // dest buffer
        wide_cmd_str_size // dest buffer size
    );

    if (status == 0) {
        fprintf(stderr, "Cannot re-encode command %s\n", cmd_str);
        exit(1);
    }

    if (capture_mode & CAPTURE_MODE_KEEP_STDOUT
        && capture_mode & CAPTURE_MODE_KEEP_STDERR
        && !(capture_mode & CAPTURE_MODE_MERGE_STDERR)) {
        // TODO: implement multithreaded approach
        fprintf(stderr, "Seperate capturing of stdout and stderr is not currently supported on Windows\n");
        exit(1);
    }

    HANDLE par_stdout = NULL;
    HANDLE sub_stdout = NULL;

    HANDLE par_stderr = NULL;
    HANDLE sub_stderr = NULL;

    if (capture_mode & CAPTURE_MODE_KEEP_STDOUT) {
        make_pipe_pair(&par_stdout, &sub_stdout);
    }

    if (capture_mode & CAPTURE_MODE_KEEP_STDERR) {
        if (capture_mode & CAPTURE_MODE_MERGE_STDERR) {
            sub_stderr = sub_stdout;
        } else {
            make_pipe_pair(&par_stderr, &sub_stderr);
        }
    }

    STARTUPINFOW start_info;
    ZeroMemory(&start_info, sizeof(start_info));
    start_info.cb = sizeof(start_info);
    start_info.dwFlags = STARTF_USESTDHANDLES;
    start_info.hStdOutput = sub_stdout;
    start_info.hStdError = sub_stderr;

    PROCESS_INFORMATION process_info;
    ZeroMemory(&process_info, sizeof(process_info));

    BOOL success = CreateProcessW(
        NULL, // application name
        wide_cmd_str,
        NULL, // process attributes
        NULL, // thread attributes
        TRUE, // inherit handles
        0, // creation flags
        NULL, // environment
        NULL, // current directory
        &start_info,
        &process_info
    );

    if (!success) {
        fprintf(stderr, "Failed to start subprocess %s\n", cmd);
        exit(1);
    }

    if (sub_stdout != NULL) CloseHandle(sub_stdout);
    if (sub_stderr != NULL) CloseHandle(sub_stderr);

    struct CapturedText captured_stdout = {0};
    struct CapturedText captured_stderr = {0};

    if (capture_mode & CAPTURE_MODE_KEEP_STDOUT) {
        capture_pipe_out(par_stdout, &captured_stdout);
    } else if (capture_mode & CAPTURE_MODE_KEEP_STDERR) {
        capture_pipe_out(par_stderr, &captured_stderr);
    }

    if (par_stdout != NULL) CloseHandle(par_stdout);
    if (par_stderr != NULL) CloseHandle(par_stderr);

    WaitForSingleObject(process_info.hProcess, INFINITE);

    DWORD exit_status = 1;
    GetExitCodeProcess(process_info.hProcess, &exit_status);

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    struct CRCProcessResult result;
    result.out = captured_text_to_str(&captured_stdout);
    result.err = captured_text_to_str(&captured_stderr);
    result.status = exit_status;
    return result;
}

#else // _MSC_VER

#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

#if __STDC_VERSION__ < 202311L
#define noreturn _Noreturn
#endif

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
    } else {
        fd->revents = 0;
        return false;
    }
}

static void parent_proc(pid_t child_pid, uint32_t capture_mode, int stdout_pipe, int stderr_pipe, struct CRCProcessResult *result) {
    struct CapturedText out = {0, 0, NULL};
    struct CapturedText err = {0, 0, NULL};

    short notable_events = POLLIN;
    struct pollfd fds[2];
    struct CapturedText *txts[2];
    nfds_t fd_count = 0;

    if (capture_mode & CAPTURE_MODE_KEEP_STDOUT) {
        fds[fd_count] = (struct pollfd) {stdout_pipe, notable_events, 0};
        txts[fd_count] = &out;
        fd_count++;
    }

    if (capture_mode & CAPTURE_MODE_MERGE_STDERR) {
        fds[fd_count] = (struct pollfd) {stderr_pipe, notable_events, 0};
        txts[fd_count] = capture_mode & CAPTURE_MODE_MERGE_STDERR ? &out : &err;
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

DLL_EXPORT
struct CRCProcessResult CRC_run_command(char *cmd, char **args, uint32_t arg_count, uint32_t capture_mode) {
    verify_capture_mode(capture_mode);

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

        struct CRCProcessResult result;
        parent_proc(pid, capture_mode, stdout_pipe[0], stderr_pipe[0], &result);
        return result;
    }
}

#endif

typedef struct ProcessResult EPSLProcessResult;

static void *safe_malloc(size_t amount) {
    void *result = malloc(amount);
    if (!result) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return result;
}

static char *c_str_from_epsl_str(struct ARRAY_char *str) {
    char *result = safe_malloc(str->length + 1);
    memcpy(result, str->content, str->length);
    result[str->length] = '\0';
    return result;
}

static struct ARRAY_char *wrap_c_str_to_epsl_str(uint64_t ref_counter, char *c_str) {
    uint64_t str_len = strlen(c_str);

    struct ARRAY_char *epsl_str = safe_malloc(sizeof(*epsl_str));
    epsl_str->ref_counter = ref_counter;
    epsl_str->capacity = str_len + 1;
    epsl_str->length = str_len;
    epsl_str->content = (unsigned char*)c_str;

    return epsl_str;
}

DLL_EXPORT
EPSLProcessResult *CRC_epsl_run_command(struct ARRAY_char *cmd, struct ARRAY_ARRAY_char *args, uint32_t capture_mode) {
    char *cmd_c_str = c_str_from_epsl_str(cmd);

    char *args_buffer[args->length];
    for (uint64_t i = 0; i < args->length; i++) {
        args_buffer[i] = c_str_from_epsl_str(args->content[i]);
    }

    struct CRCProcessResult result = CRC_run_command(
        cmd_c_str, args_buffer, args->length, capture_mode);
    
    free(cmd_c_str);
    for (uint64_t i = 0; i < args->length; i++) {
        free(args_buffer[i]);
    }

    EPSLProcessResult *epsl_result = safe_malloc(sizeof(*epsl_result));
    epsl_result->ref_counter = 0;
    epsl_result->out = wrap_c_str_to_epsl_str(1, result.out);
    epsl_result->err = wrap_c_str_to_epsl_str(1, result.err);
    epsl_result->status = result.status;

    return epsl_result;
}
