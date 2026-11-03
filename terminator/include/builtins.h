#ifndef BUILTINS_H
#define BUILTINS_H

#include "terminator.h"
#include "jobcontrol.h"

typedef struct TabState TabState;

int builtin_is_builtin(const char *cmd);
int builtin_execute(TabState *tab, char **args, int arg_count, int out_fd, int in_fd);

#endif /* BUILTINS_H */
