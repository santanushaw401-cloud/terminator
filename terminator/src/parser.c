/*
 * ==============================================================================
 * Project:      Terminator - A Custom Linux Terminal & Shell
 * File:         src/parser.c
 * Purpose:      Lexical analysis, command line parsing, and shell expansions.
 * Description:  Transforms raw terminal input lines into structured Pipeline objects.
 *               Handles:
 *               1. Multiline detection (incomplete quotes, unclosed $(), trailing \).
 *               2. Command substitution (`cmd` and $(cmd)).
 *               3. Parameter expansion ($VAR and exit code $?).
 *               4. Input/output redirection (<, >, >>).
 *               5. Arbitrary pipelines (cmd1 | cmd2 | cmd3 ...).
 *               6. Background execution flag (&).
 *
 * Viva / Examination Explanations:
 * 1. How does the shell detect multiline input?
 *    - parser_is_incomplete() runs a state machine tracking whether the user has
 *      an unclosed single quote, unclosed double quote, unclosed $(, or a trailing
 *      backslash ('\'). If so, the shell doesn't execute; it prints a secondary prompt '> '
 *      and waits for the user to complete the command.
 * 2. Why do single quotes ('...') differ from double quotes ("...")?
 *    - Single quotes preserve the literal value of every character inside them ($VAR is NOT expanded).
 *    - Double quotes suppress word splitting and globbing, but ALLOW variable expansion ($VAR is replaced).
 * 3. How does command substitution $(...) work?
 *    - The shell forks a child process running /bin/sh -c "subcmd", captures its standard output
 *      via a pipe into memory, strips trailing newlines, and splices the output text into the command line!
 * ==============================================================================
 */

#include "parser.h" /* Declarations for Pipeline, SimpleCommand, and parser functions */

/*
 * Function:    parser_is_incomplete
 * Purpose:     Checks if a typed line has unclosed quotes, unclosed parentheses, or trailing backslash.
 * Parameters:  input - Raw input string typed by user.
 * Returns:     int   - 1 if input is incomplete (needs multiline continuation), 0 if complete.
 */
int parser_is_incomplete(const char *input) {
    if (!input) return 0; /* Null input is complete */

    int in_single_quote = 0; /* True (1) when currently inside '...' */
    int in_double_quote = 0; /* True (1) when currently inside "..." */
    int in_backtick = 0;     /* True (1) when currently inside `...` */
    int paren_depth = 0;     /* Nesting depth of command substitutions $(...) */
    int escaped = 0;         /* True (1) if preceding character was an unescaped backslash '\' */

    int len = strlen(input);
    for (int i = 0; i < len; i++) {
        char c = input[i];

        /* If previous character was an escape backslash, treat current character as literal */
        if (escaped) {
            escaped = 0;
            continue;
        }

        /* An unescaped backslash escapes the next character (unless inside single quotes) */
        if (c == '\\' && !in_single_quote) {
            escaped = 1;
            continue;
        }

        /* Inside single quotes: everything is literal until the matching closing single quote */
        if (in_single_quote) {
            if (c == '\'') {
                in_single_quote = 0; /* Closed single quote */
            }
        } 
        /* Inside double quotes: permits nested command substitution and backticks */
        else if (in_double_quote) {
            if (c == '"') {
                in_double_quote = 0; /* Closed double quote */
            } else if (c == '`') {
                in_backtick = !in_backtick;
            } else if (c == '$' && i + 1 < len && input[i + 1] == '(') {
                paren_depth++; /* Opening $( */
                i++;
            } else if (c == ')' && paren_depth > 0) {
                paren_depth--; /* Closing ) */
            }
        } 
        /* Outside any quotes */
        else {
            if (c == '\'') {
                in_single_quote = 1; /* Opening single quote */
            } else if (c == '"') {
                in_double_quote = 1; /* Opening double quote */
            } else if (c == '`') {
                in_backtick = !in_backtick;
            } else if (c == '$' && i + 1 < len && input[i + 1] == '(') {
                paren_depth++; /* Opening $( */
                i++;
            } else if (c == ')' && paren_depth > 0) {
                paren_depth--; /* Closing ) */
            }
        }
    }

    /*
     * If at the end of the line any structure remains unclosed, or the line ended
     * with an unescaped backslash ('\'), the command is incomplete!
     */
    if (escaped) return 1;          /* Trailing backslash -> line continuation */
    if (in_single_quote) return 1;  /* Missing closing ' */
    if (in_double_quote) return 1;  /* Missing closing " */
    if (in_backtick) return 1;      /* Missing closing ` */
    if (paren_depth > 0) return 1;  /* Missing closing ) */

    return 0; /* Command is syntactically complete */
}

/*
 * Function:    run_command_capture
 * Purpose:     Executes a sub-command for command substitution and captures its stdout into memory.
 * Parameters:  cmd - Sub-command string to execute (e.g., "whoami" or "date").
 *              cwd - Current working directory where the sub-command should run.
 * Returns:     char* - Heap-allocated string containing output text (caller must free()).
 */
static char *run_command_capture(const char *cmd, const char *cwd) {
    int pfd[2];
    /* Create pipe to capture child's standard output */
    if (pipe(pfd) < 0) return strdup("");

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return strdup("");
    }

    if (pid == 0) {
        /* ================= Child Process ================= */
        if (cwd && strlen(cwd) > 0) {
            chdir(cwd);
        }
        close(pfd[0]); /* Child does not read; close read end */
        /* Redirect standard output (1) into the pipe write end */
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);

        /* Execute sub-command using POSIX shell */
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        exit(127); /* If exec fails, exit child with 127 */
    }

    /* ================= Parent Process ================= */
    close(pfd[1]); /* Parent does not write; close write end */
    size_t cap = 4096;
    size_t size = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        close(pfd[0]);
        waitpid(pid, NULL, 0);
        return strdup("");
    }

    /* Read child output until EOF */
    ssize_t n;
    while ((n = read(pfd[0], buf + size, cap - size - 1)) > 0) {
        size += n;
        if (size + 512 >= cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
        }
    }
    close(pfd[0]);
    waitpid(pid, NULL, 0); /* Reap child process */

    buf[size] = '\0';
    /* Strip trailing newlines (standard bash behavior for command substitution) */
    while (size > 0 && (buf[size - 1] == '\n' || buf[size - 1] == '\r')) {
        buf[size - 1] = '\0';
        size--;
    }
    return buf;
}

/*
 * Function:    expand_command_substitutions
 * Purpose:     Recursively finds and expands all $(...) and `...` command substitutions.
 * Parameters:  input - Raw input string.
 *              cwd   - Current working directory.
 * Returns:     char* - Newly allocated string with substitutions resolved (caller must free()).
 */
static char *expand_command_substitutions(const char *input, const char *cwd) {
    char *curr = strdup(input);
    if (!curr) return NULL;

    while (1) {
        int len = strlen(curr);
        int innermost_start = -1;
        int innermost_end = -1;
        int type = 0; /* 1 = $(...), 2 = `...` */
        int in_sq = 0;

        /* Scan for innermost substitution (handles nested substitutions from inside out) */
        for (int i = 0; i < len; i++) {
            if (curr[i] == '\'' && (i == 0 || curr[i - 1] != '\\')) {
                in_sq = !in_sq;
                continue;
            }
            if (in_sq) continue; /* Single quotes protect against substitution */

            if (curr[i] == '$' && i + 1 < len && curr[i + 1] == '(') {
                innermost_start = i;
                type = 1;
            } else if (curr[i] == '`' && (i == 0 || curr[i - 1] != '\\')) {
                if (type == 2 && innermost_start >= 0) {
                    innermost_end = i;
                    break;
                } else {
                    innermost_start = i;
                    type = 2;
                }
            } else if (curr[i] == ')' && type == 1 && innermost_start >= 0) {
                innermost_end = i;
                break;
            }
        }

        if (innermost_start < 0 || innermost_end < 0) {
            break; /* No more unexpanded command substitutions */
        }

        /* Extract the sub-command string inside $(...) or `...` */
        char subcmd[MAX_CMD_LEN] = {0};
        if (type == 1) {
            int sub_len = innermost_end - (innermost_start + 2);
            if (sub_len >= (int)sizeof(subcmd)) sub_len = sizeof(subcmd) - 1;
            strncpy(subcmd, curr + innermost_start + 2, sub_len);
        } else {
            int sub_len = innermost_end - (innermost_start + 1);
            if (sub_len >= (int)sizeof(subcmd)) sub_len = sizeof(subcmd) - 1;
            strncpy(subcmd, curr + innermost_start + 1, sub_len);
        }

        /* Run sub-command and capture its output */
        char *output = run_command_capture(subcmd, cwd);

        /* Splice the captured output back into the string replacing $(...) */
        int out_len = strlen(output);
        int new_len = innermost_start + out_len + (len - innermost_end - 1) + 1;
        char *next = (char *)malloc(new_len);
        strncpy(next, curr, innermost_start);
        next[innermost_start] = '\0';
        strcat(next, output);
        strcat(next, curr + innermost_end + 1);

        free(output);
        free(curr);
        curr = next;
    }

    return curr;
}

/*
 * Function:    parser_expand
 * Purpose:     Expands command substitutions, environment variables ($VAR), exit code ($?), and line continuations.
 * Parameters:  input       - Input command string.
 *              last_status - Exit status of the previous command (used for $?).
 *              cwd         - Current working directory.
 * Returns:     char*       - Heap-allocated expanded string (caller must free()).
 */
char *parser_expand(const char *input, int last_status, const char *cwd) {
    if (!input) return NULL;

    /* 1. First expand all $(cmd) and `cmd` command substitutions */
    char *with_subst = expand_command_substitutions(input, cwd);
    if (!with_subst) with_subst = strdup(input);

    /* 2. Expand $VAR, $?, and line continuations */
    size_t out_cap = strlen(with_subst) * 2 + 512;
    char *out = (char *)malloc(out_cap);
    if (!out) {
        free(with_subst);
        return NULL;
    }

    int in_sq = 0;
    int in_dq = 0;
    int out_idx = 0;
    int len = strlen(with_subst);

    for (int i = 0; i < len; i++) {
        char c = with_subst[i];

        /* Line continuation: backslash followed immediately by newline is stripped */
        if (c == '\\' && i + 1 < len && with_subst[i + 1] == '\n') {
            i++; /* Skip both '\' and '\n' */
            continue;
        }

        /* Single quote toggle (suppresses variable expansion) */
        if (c == '\'' && !in_dq) {
            in_sq = !in_sq;
            out[out_idx++] = c;
            continue;
        }

        /* Double quote toggle */
        if (c == '"' && !in_sq) {
            in_dq = !in_dq;
            out[out_idx++] = c;
            continue;
        }

        /* Check for $ expansions (only allowed outside single quotes) */
        if (c == '$' && !in_sq && i + 1 < len) {
            /* Case: $? -> expands to exit code of last command */
            if (with_subst[i + 1] == '?') {
                char stat_str[16];
                snprintf(stat_str, sizeof(stat_str), "%d", last_status);
                int slen = strlen(stat_str);
                while (out_idx + slen >= (int)out_cap) {
                    out_cap *= 2;
                    out = (char *)realloc(out, out_cap);
                }
                strcpy(out + out_idx, stat_str);
                out_idx += slen;
                i++; /* Skip '?' */
                continue;
            } 
            /* Case: $VAR -> expands to environment variable value */
            else if (isalpha((unsigned char)with_subst[i + 1]) || with_subst[i + 1] == '_') {
                int var_start = i + 1;
                int var_end = var_start;
                while (var_end < len && (isalnum((unsigned char)with_subst[var_end]) || with_subst[var_end] == '_')) {
                    var_end++;
                }
                char var_name[128] = {0};
                int var_len = var_end - var_start;
                if (var_len < (int)sizeof(var_name)) {
                    strncpy(var_name, with_subst + var_start, var_len);
                }
                const char *val = getenv(var_name); /* Look up in environment table */
                if (val) {
                    int vlen = strlen(val);
                    while (out_idx + vlen >= (int)out_cap) {
                        out_cap *= 2;
                        out = (char *)realloc(out, out_cap);
                    }
                    strcpy(out + out_idx, val);
                    out_idx += vlen;
                }
                i = var_end - 1;
                continue;
            }
        }

        /* Ensure sufficient output buffer capacity */
        if (out_idx + 8 >= (int)out_cap) {
            out_cap *= 2;
            out = (char *)realloc(out, out_cap);
        }
        out[out_idx++] = c;
    }

    out[out_idx] = '\0';
    free(with_subst);
    return out;
}

/*
 * Function:    parse_command_args_and_redir
 * Purpose:     Parses a single command chunk into argument array and redirection targets.
 * Parameters:  raw_cmd - Single command string without pipe operators (e.g., "cat < file.txt > out.txt").
 *              sc      - Pointer to SimpleCommand structure to populate.
 * Returns:     void.
 *
 * Handles:
 * - Tokenizes arguments on whitespace while preserving quoted text ("hello world").
 * - Strips enclosing quotes.
 * - Extracts input redirection: `< filename`.
 * - Extracts output redirection: `> filename` (overwrite) and `>> filename` (append).
 */
static void parse_command_args_and_redir(char *raw_cmd, SimpleCommand *sc) {
    memset(sc, 0, sizeof(SimpleCommand));

    int len = strlen(raw_cmd);
    char cur_token[MAX_CMD_LEN];
    int cur_len = 0;
    int in_sq = 0;
    int in_dq = 0;
    int redir_mode = 0; /* 0 = none, 1 = '<', 2 = '>', 3 = '>>' */

    for (int i = 0; i <= len; i++) {
        char c = raw_cmd[i];

        /* Strip single quotes */
        if (c == '\'' && !in_dq) {
            in_sq = !in_sq;
            continue;
        }
        /* Strip double quotes */
        if (c == '"' && !in_sq) {
            in_dq = !in_dq;
            continue;
        }

        /* Delimiter check outside quotes: space, tab, redirection symbols, or end of string */
        if (!in_sq && !in_dq && (c == ' ' || c == '\t' || c == '<' || c == '>' || c == '\0')) {
            if (cur_len > 0) {
                cur_token[cur_len] = '\0';
                if (redir_mode == 1) {
                    sc->infile = strdup(cur_token); /* Input file redirection target */
                    redir_mode = 0;
                } else if (redir_mode == 2 || redir_mode == 3) {
                    sc->outfile = strdup(cur_token); /* Output file redirection target */
                    sc->append_out = (redir_mode == 3); /* Append mode if >> */
                    redir_mode = 0;
                } else {
                    /* Regular argument token */
                    if (sc->arg_count < MAX_ARGS - 1) {
                        sc->args[sc->arg_count++] = strdup(cur_token);
                    }
                }
                cur_len = 0;
            }

            /* Detect redirection operators */
            if (c == '<') {
                redir_mode = 1; /* Input redirect */
            } else if (c == '>') {
                if (i + 1 < len && raw_cmd[i + 1] == '>') {
                    redir_mode = 3; /* Append output redirect '>>' */
                    i++;            /* Skip second '>' */
                } else {
                    redir_mode = 2; /* Truncate output redirect '>' */
                }
            }
        } else {
            /* Accumulate characters inside current token */
            if (c != '\0' && cur_len < MAX_CMD_LEN - 1) {
                cur_token[cur_len++] = c;
            }
        }
    }
    sc->args[sc->arg_count] = NULL; /* Null-terminate argument array for execvp() */
}

/*
 * Function:    parser_parse_pipeline
 * Purpose:     Splits a full command line into a Pipeline containing multiple SimpleCommands chained by '|'.
 * Parameters:  input - Fully expanded input command line.
 *              pl    - Pointer to Pipeline structure to populate.
 * Returns:     int   - 0 on success, -1 on invalid input.
 */
int parser_parse_pipeline(const char *input, Pipeline *pl) {
    if (!input || !pl) return -1;
    memset(pl, 0, sizeof(Pipeline));
    strncpy(pl->raw_cmd, input, sizeof(pl->raw_cmd) - 1);

    /*
     * Check for trailing background operator '&':
     * If the command ends with '&', mark is_background = 1 and strip '&'.
     */
    char buf[MAX_CMD_LEN];
    strncpy(buf, input, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int blen = strlen(buf);
    while (blen > 0 && (buf[blen - 1] == ' ' || buf[blen - 1] == '\t' || buf[blen - 1] == '\n')) {
        buf[--blen] = '\0';
    }
    if (blen > 0 && buf[blen - 1] == '&') {
        pl->is_background = 1;
        buf[--blen] = '\0';
    }

    /*
     * Split command line by pipe '|' symbols outside quotes:
     * e.g., "cat file | grep text | sort" -> 3 command parts.
     */
    char cmd_parts[MAX_PIPES][MAX_CMD_LEN];
    int part_count = 0;
    int part_len = 0;
    int in_sq = 0;
    int in_dq = 0;

    for (int i = 0; i <= blen; i++) {
        char c = buf[i];

        if (c == '\'' && !in_dq) in_sq = !in_sq;
        if (c == '"' && !in_sq) in_dq = !in_dq;

        /* Pipe character outside quotes indicates boundary between commands */
        if ((c == '|' || c == '\0') && !in_sq && !in_dq) {
            cmd_parts[part_count][part_len] = '\0';
            if (part_len > 0 && part_count < MAX_PIPES) {
                part_count++;
            }
            part_len = 0;
        } else {
            if (c != '\0' && part_len < MAX_CMD_LEN - 1) {
                cmd_parts[part_count][part_len++] = c;
            }
        }
    }

    pl->cmd_count = part_count;
    /* Parse arguments, redirections, and options for each stage of the pipeline */
    for (int i = 0; i < part_count; i++) {
        parse_command_args_and_redir(cmd_parts[i], &pl->commands[i]);
    }

    return 0;
}

/*
 * Function:    pipeline_free
 * Purpose:     Deallocates all dynamically allocated strings inside a Pipeline structure.
 * Parameters:  pl - Pointer to Pipeline to free.
 * Returns:     void.
 */
void pipeline_free(Pipeline *pl) {
    if (!pl) return;
    for (int i = 0; i < pl->cmd_count; i++) {
        SimpleCommand *sc = &pl->commands[i];
        for (int a = 0; a < sc->arg_count; a++) {
            if (sc->args[a]) {
                free(sc->args[a]);
                sc->args[a] = NULL;
            }
        }
        if (sc->infile) {
            free(sc->infile);
            sc->infile = NULL;
        }
        if (sc->outfile) {
            free(sc->outfile);
            sc->outfile = NULL;
        }
    }
    pl->cmd_count = 0;
}
