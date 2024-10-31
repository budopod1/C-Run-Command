#ifndef RUNCOMMAND_H
#define RUNCOMMAND_H

#include <stdint.h>

union ___EPSL_PUBLIC_STOP;
struct CRCProcessResult {
    char *output;
    unsigned char status;
};
union ___EPSL_PUBLIC_START;

struct ARRAY_char {
    uint64_t ref_counter;
    uint64_t capacity;
    uint64_t length;
    char *content;
}

struct ARRAY_ARRAY_char {
    uint64_t ref_counter;
    uint64_t capacity;
    uint64_t length;
    struct ARRAY_char *content;
}

struct ProcessResult {
    uint64_t ref_counter;
    struct ARRAY_char *output;
    uint8_t status;
}

union ___EPSL_PUBLIC_STOP;
struct CRCProcessResult CRC_run_command(char *cmd, char **args, uint32_t arg_count);
union ___EPSL_PUBLIC_START;

struct ProcessResult *CRC_epsl_run_command(struct ARRAY_char *cmd, struct ARRAY_ARRAY_char *args);

#endif
