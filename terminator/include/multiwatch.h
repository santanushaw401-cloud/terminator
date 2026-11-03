#ifndef MULTIWATCH_H
#define MULTIWATCH_H

#include "terminator.h"

#define MAX_WATCH_CMDS 32

typedef struct {
    char cmd[MAX_CMD_LEN];
    pid_t pid;
    int pipe_fd;
    int is_running;
    time_t finish_time;
    char output[32768];
    int out_len;
} WatchedCommand;

int multiwatch_parse_args(const char *arg_str, char cmds[][MAX_CMD_LEN], int max_cmds);
void multiwatch_run(char cmds[][MAX_CMD_LEN], int cmd_count, int out_fd, int in_fd, volatile int *stop_flag, const char *cwd);

#endif /* MULTIWATCH_H */
