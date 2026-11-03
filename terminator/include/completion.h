#ifndef COMPLETION_H
#define COMPLETION_H

#include "terminator.h"

#define MAX_COMPLETION_MATCHES 64
#define MAX_FILENAME_LEN 256

typedef struct {
    int match_count;
    char matches[MAX_COMPLETION_MATCHES][MAX_FILENAME_LEN];
    char common_prefix[MAX_FILENAME_LEN];
    char dir_part[MAX_PATH_LEN];
    int word_start;
    int word_end;
} CompletionResult;

int complete_word(const char *line, int cursor_pos, const char *cwd, CompletionResult *res);
const char *longest_common_prefix(char matches[][MAX_FILENAME_LEN], int count, char *out, int max_len);

#endif /* COMPLETION_H */
