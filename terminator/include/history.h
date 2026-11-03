#ifndef HISTORY_H
#define HISTORY_H

#include "terminator.h"

typedef struct {
    sem_t sem;
    int count;
    char commands[MAX_HISTORY_TOTAL][MAX_CMD_LEN];
} SharedHistory;

void history_init(ExecutionMode mode);
void history_cleanup(void);
void history_add(const char *cmd);
int history_count(void);
int history_get(int index, char *out, int max_len);
void history_print_recent(int n, char *out_buf, int max_buf_len);
void history_search_ctrl_r(const char *term, char *out_buf, int max_buf_len);
void history_clear(void);

#endif /* HISTORY_H */
