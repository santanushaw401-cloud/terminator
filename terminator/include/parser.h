#ifndef PARSER_H
#define PARSER_H

#include "terminator.h"

typedef struct {
    char *args[MAX_ARGS];
    int arg_count;
    char *infile;
    char *outfile;
    int append_out;
} SimpleCommand;

typedef struct {
    SimpleCommand commands[MAX_PIPES];
    int cmd_count;
    int is_background;
    char raw_cmd[MAX_CMD_LEN];
} Pipeline;

int parser_is_incomplete(const char *input);
char *parser_expand(const char *input, int last_status, const char *cwd);
int parser_parse_pipeline(const char *input, Pipeline *pl);
void pipeline_free(Pipeline *pl);

#endif /* PARSER_H */
