#ifndef RUNCOMMAND_H
#define RUNCOMMAND_H

#include <stdint.h>

#if defined(EPSL_PROJECT) || defined(REQUIRE_FULL_RUNCOMMAND_INTERFACE)

#define EPSL_COMMON_PREFIX "CRC_epsl_"
#define EPSL_IMPLEMENTATION_LOCATION "runcommand.c"

struct ARRAY_char {
    uint64_t ref_counter;
    uint64_t capacity;
    uint64_t length;
    unsigned char *content;
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

struct ProcessResult *CRC_epsl_run_command(struct ARRAY_char *cmd, struct ARRAY_ARRAY_char *args, uint32_t capture_mode);

#endif

#if !defined(EPSL_PROJECT) || defined(REQUIRE_FULL_RUNCOMMAND_INTERFACE)

struct CRCProcessResult {
    char *out;
    char *err;
    unsigned char status;
};

struct CRCProcessResult CRC_run_command(char *cmd, char **args, uint32_t arg_count, uint32_t capture_mode);

#endif

#endif
