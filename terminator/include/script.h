#ifndef SCRIPT_H
#define SCRIPT_H

#include "terminator.h"

typedef struct {
    char name[64];
    char val[MAX_CMD_LEN];
} ScriptVar;

typedef struct {
    ScriptVar vars[128];
    int var_count;
    char args[32][MAX_CMD_LEN];
    int arg_count;
    int last_status;
    char cwd[MAX_PATH_LEN];
} ScriptContext;

int script_validate_syntax(const char *filepath, char *err_buf, int max_err_len);
int script_execute_file(const char *filepath, int argc, char **argv, int out_fd, int in_fd, const char *cwd, int *last_status);

#endif /* SCRIPT_H */
