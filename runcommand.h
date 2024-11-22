#ifndef RUNCOMMAND_H
#define RUNCOMMAND_H

#include <stdint.h>

union ___EPSL_PUBLIC_STOP;
struct CRCProcessResult {
    char *out;
    char *err;
    unsigned char status;
};

struct CRCCaptureSettings {
    unsigned int capture_stdout : 1;
    unsigned int capture_stderr : 1;
    unsigned int merge_stderr: 1;
};
union ___EPSL_PUBLIC_START;

struct ARRAY_char {
    uint64_t ref_counter;
    uint64_t capacity;
    uint64_t length;
    char *content;
};

struct ARRAY_ARRAY_char {
    uint64_t ref_counter;
    uint64_t capacity;
    uint64_t length;
    struct ARRAY_char **content;
};

struct ProcessResult {
    uint64_t ref_counter;
    struct ARRAY_char *out;
    struct ARRAY_char *err;
    uint8_t status;
};

union ___EPSL_PUBLIC_STOP;
struct CRCProcessResult CRC_run_command(char *cmd, char **args, uint32_t arg_count, struct CRCCaptureSettings settings);
union ___EPSL_PUBLIC_START;

// TODO: if the function matches ^[A-Z]+_epsl_([A-Za-z0-9_]+)$, the exposed funciton name in Epsilon
// should be the capture group
struct ProcessResult *CRC_epsl_run_command(struct ARRAY_char *cmd, struct ARRAY_ARRAY_char *args, uint32_t capture_mode);

#endif
