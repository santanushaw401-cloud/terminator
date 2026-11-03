/*
 * =====================================================================================
 * File: src/script.c
 * Project: Terminator - Custom Tabbed Terminal Emulator & Shell
 *
 * Educational Purpose & Viva Guide:
 * This file implements a built-in shell script interpreter capable of executing
 * bash-like shell scripts (.sh files). It provides:
 *   1. Pre-execution syntax validation (checking balanced if/elif/else/fi constructs).
 *   2. Variable storage and expansion (positional parameters $0..$9, $#, $?, and custom variables).
 *   3. Expression testing ([ -f file ], [ -d dir ], [ -e path ], [ a = b ], [ x -eq y ]).
 *   4. Control flow state machine with an execution stack (IfFrame stack) to handle nested
 *      if-elif-else-fi blocks correctly.
 *
 * System Calls & Libraries Used:
 *   - <stdio.h>: fopen(), fgets(), fclose(), snprintf() for stream I/O and formatted strings.
 *   - <stdlib.h>: getenv(), atoi() for reading environment variables and ASCII-to-integer conversion.
 *   - <string.h>: strncmp(), strcmp(), strlen(), strchr(), strrchr(), strncpy(), memmove().
 *   - <ctype.h>: isdigit(), isalpha(), isalnum() for character classification during parsing.
 *   - <unistd.h>: access(), F_OK, write() for filesystem access checks and direct descriptor writing.
 *   - <sys/stat.h>: stat(), S_ISREG(), S_ISDIR() for inspecting file metadata and type checking.
 * =====================================================================================
 */

#include "script.h"      /* Header declaring ScriptContext, script_validate_syntax(), script_execute_file() */
#include "shell.h"       /* Header for shell execution primitives (shell_execute_line, TabState, etc.) */
#include "parser.h"      /* Header containing parsing definitions and buffer limits */

/*
 * -------------------------------------------------------------------------------------
 * Function: script_validate_syntax
 *
 * Purpose:
 *   Performs a fast first-pass static analysis of a script file to catch unbalanced
 *   conditional blocks (if/elif/else/fi) before executing any commands. This prevents
 *   partial script execution where half the commands run before hitting a syntax error.
 *
 * Parameters:
 *   @filepath:    Relative or absolute path to the script file to check.
 *   @err_buf:     Output buffer where human-readable error messages are written if an error is found.
 *   @max_err_len: Size of err_buf in bytes to prevent buffer overflow.
 *
 * Returns:
 *   int: 0 if the syntax is valid (all if blocks properly closed with fi).
 *       -1 if a syntax error occurred (e.g., unexpected elif/else/fi, unclosed if, or file unreadable).
 * -------------------------------------------------------------------------------------
 */
int script_validate_syntax(const char *filepath, char *err_buf, int max_err_len) {
    /* fopen(): Open the script file in read-only mode ("r") */
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        /* If fopen returns NULL, the file does not exist or permission is denied */
        if (err_buf) {
            snprintf(err_buf, max_err_len, "terminator: %s: No such file or directory\n", filepath);
        }
        return -1; /* Return -1 indicating failure */
    }

    char line[MAX_LINE_LEN]; /* Buffer to hold each line read from the script */
    int line_num = 0;        /* Track line number for clear syntax error reporting */
    int if_depth = 0;        /* Counter tracking the nesting depth of 'if' blocks */

    /* fgets(): Read line-by-line until End-Of-File (EOF) is reached */
    while (fgets(line, sizeof(line), fp)) {
        line_num++;          /* Increment line counter */

        /* Skip leading whitespace (spaces and tabs) */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        /* If line starts with '#' (comment), '\n' (empty), or '\0' (blank), skip it */
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        /* Check for starting an 'if' block */
        if (strncmp(p, "if ", 3) == 0 || strncmp(p, "if\t", 3) == 0) {
            if_depth++; /* Increase nesting level */
        }
        /* Check for 'elif' keyword */
        else if (strncmp(p, "elif ", 5) == 0 || strncmp(p, "elif\t", 5) == 0) {
            /* 'elif' is only legal if we are currently inside an 'if' block */
            if (if_depth <= 0) {
                if (err_buf) snprintf(err_buf, max_err_len, "terminator: %s:%d: syntax error: unexpected 'elif'\n", filepath, line_num);
                fclose(fp); /* Close open file handle to avoid resource leak */
                return -1;  /* Syntax error detected */
            }
        }
        /* Check for 'else' keyword */
        else if (strncmp(p, "else", 4) == 0 && (p[4] == ' ' || p[4] == '\t' || p[4] == '\n' || p[4] == ';')) {
            /* 'else' is only legal if we are currently inside an 'if' block */
            if (if_depth <= 0) {
                if (err_buf) snprintf(err_buf, max_err_len, "terminator: %s:%d: syntax error: unexpected 'else'\n", filepath, line_num);
                fclose(fp); /* Close open file handle */
                return -1;  /* Syntax error detected */
            }
        }
        /* Check for 'fi' keyword (closing an 'if' block) */
        else if (strncmp(p, "fi", 2) == 0 && (p[2] == ' ' || p[2] == '\t' || p[2] == '\n' || p[2] == ';' || p[2] == '\0')) {
            if_depth--; /* Decrement nesting level */
            /* If if_depth drops below 0, 'fi' was found without a matching 'if' */
            if (if_depth < 0) {
                if (err_buf) snprintf(err_buf, max_err_len, "terminator: %s:%d: syntax error: unexpected 'fi'\n", filepath, line_num);
                fclose(fp); /* Close open file handle */
                return -1;  /* Syntax error detected */
            }
        }
    }
    fclose(fp); /* Finished scanning entire file, close file pointer */

    /* If if_depth > 0, an 'if' block was opened but never closed with 'fi' */
    if (if_depth > 0) {
        if (err_buf) snprintf(err_buf, max_err_len, "terminator: %s: syntax error: unexpected end of file, expected 'fi'\n", filepath);
        return -1; /* Syntax error: unclosed if block */
    }

    return 0; /* Syntax is completely valid */
}

/*
 * -------------------------------------------------------------------------------------
 * Function: get_var (static helper)
 *
 * Purpose:
 *   Retrieves the string value of a variable by name.
 *   First searches the local script context's variable array (ctx->vars).
 *   If not found locally, falls back to the system environment via getenv().
 *
 * Parameters:
 *   @ctx:  Pointer to current ScriptContext holding local script state and variables.
 *   @name: Name of the variable to find (e.g., "MY_VAR" or "PATH").
 *
 * Returns:
 *   const char*: Pointer to the variable's string value, or NULL if not found anywhere.
 * -------------------------------------------------------------------------------------
 */
static const char *get_var(ScriptContext *ctx, const char *name) {
    /* 1. Search local script context variables */
    for (int i = 0; i < ctx->var_count; i++) {
        if (strcmp(ctx->vars[i].name, name) == 0) {
            return ctx->vars[i].val; /* Found in script context */
        }
    }
    /* 2. Fall back to process environment variables via getenv() */
    return getenv(name);
}

/*
 * -------------------------------------------------------------------------------------
 * Function: set_var (static helper)
 *
 * Purpose:
 *   Stores or updates a local variable in the script context.
 *   If a variable with the given name already exists, its value is overwritten.
 *   Otherwise, a new variable record is appended (up to 128 variables).
 *
 * Parameters:
 *   @ctx:  Pointer to ScriptContext.
 *   @name: Variable name string.
 *   @val:  New string value to assign to the variable.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static void set_var(ScriptContext *ctx, const char *name, const char *val) {
    /* 1. Check if variable already exists to overwrite it */
    for (int i = 0; i < ctx->var_count; i++) {
        if (strcmp(ctx->vars[i].name, name) == 0) {
            strncpy(ctx->vars[i].val, val, MAX_CMD_LEN - 1); /* Overwrite existing value */
            ctx->vars[i].val[MAX_CMD_LEN - 1] = '\0';        /* Ensure null-termination */
            return;
        }
    }
    /* 2. Add as new variable if space is available in table */
    if (ctx->var_count < 128) {
        strncpy(ctx->vars[ctx->var_count].name, name, 63); /* Copy name (max 63 chars) */
        ctx->vars[ctx->var_count].name[63] = '\0';
        strncpy(ctx->vars[ctx->var_count].val, val, MAX_CMD_LEN - 1); /* Copy value */
        ctx->vars[ctx->var_count].val[MAX_CMD_LEN - 1] = '\0';
        ctx->var_count++; /* Increment variable count */
    }
}

/*
 * -------------------------------------------------------------------------------------
 * Function: expand_script_vars (static helper)
 *
 * Purpose:
 *   Expands variables and special parameters in a script command line.
 *   Supports:
 *     - Single quotes '...': Disables variable expansion completely (literal).
 *     - $#: Count of script arguments ($#).
 *     - $?: Exit status code of the last executed command ($?).
 *     - $0 .. $9: Positional script arguments ($0 is script name, $1 is first arg, etc.).
 *     - $VAR_NAME: Custom or environment variable names (alphanumeric and underscore).
 *
 * Parameters:
 *   @ctx:      Pointer to ScriptContext holding arguments and variable table.
 *   @src:      Source string containing unexpanded text.
 *   @dst:      Destination buffer to receive the expanded string.
 *   @dst_size: Maximum capacity of the destination buffer.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static void expand_script_vars(ScriptContext *ctx, const char *src, char *dst, size_t dst_size) {
    size_t d = 0;               /* Current write index into destination buffer dst */
    size_t len = strlen(src);   /* Total length of source string */
    int in_sq = 0;              /* Flag: 1 if currently inside single quotes '...' */

    for (size_t i = 0; i < len && d < dst_size - 1; i++) {
        char c = src[i];

        /* Toggle single quote state (single quotes suppress $ expansion) */
        if (c == '\'') {
            in_sq = !in_sq;   /* Invert single-quote state */
            dst[d++] = c;     /* Preserve the quote character */
            continue;
        }

        /* Check for variable expansion marker '$' outside single quotes */
        if (c == '$' && !in_sq && i + 1 < len) {
            char next = src[i + 1];

            /* Special parameter: $# (number of script arguments) */
            if (next == '#') {
                char buf[16];
                /* Format argument count: arg_count - 1 because argv[0] is the script name itself */
                snprintf(buf, sizeof(buf), "%d", (ctx->arg_count > 0 ? ctx->arg_count - 1 : 0));
                for (size_t k = 0; k < strlen(buf) && d < dst_size - 1; k++) dst[d++] = buf[k];
                i++;          /* Skip the '#' character */
                continue;
            }
            /* Special parameter: $? (exit status of the previous command) */
            else if (next == '?') {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", ctx->last_status); /* Format status code as decimal */
                for (size_t k = 0; k < strlen(buf) && d < dst_size - 1; k++) dst[d++] = buf[k];
                i++;          /* Skip the '?' character */
                continue;
            }
            /* Positional parameter: $0, $1, ..., $9 */
            else if (isdigit((unsigned char)next)) {
                int arg_idx = next - '0'; /* Convert ASCII digit character to integer index */
                if (arg_idx < ctx->arg_count) {
                    const char *val = ctx->args[arg_idx];
                    for (size_t k = 0; k < strlen(val) && d < dst_size - 1; k++) dst[d++] = val[k];
                }
                i++;          /* Skip the digit character */
                continue;
            }
            /* Named variable: $VAR (letters or underscore followed by alphanumeric chars) */
            else if (isalpha((unsigned char)next) || next == '_') {
                size_t vstart = i + 1; /* Variable name start index */
                size_t vend = vstart;   /* Variable name end index */

                /* Find the boundary of the variable name */
                while (vend < len && (isalnum((unsigned char)src[vend]) || src[vend] == '_')) {
                    vend++;
                }

                char vname[64] = {0};
                size_t vlen = vend - vstart;
                if (vlen < sizeof(vname)) {
                    strncpy(vname, src + vstart, vlen);
                    vname[vlen] = '\0';
                }

                /* Lookup variable value via get_var (local table then environment) */
                const char *val = get_var(ctx, vname);
                if (val) {
                    for (size_t k = 0; k < strlen(val) && d < dst_size - 1; k++) dst[d++] = val[k];
                }

                i = vend - 1; /* Advance index to the end of the variable name */
                continue;
            }
        }

        /* Normal character: copy directly to destination buffer */
        dst[d++] = c;
    }
    dst[d] = '\0'; /* Ensure null-terminated string */
}

/*
 * -------------------------------------------------------------------------------------
 * Function: eval_test (static helper)
 *
 * Purpose:
 *   Evaluates conditional test expressions formatted as `[ <expr> ]`.
 *   Supports both unary and binary tests:
 *     Unary:
 *       [ -f <file> ] -> True if regular file exists (stat() + S_ISREG)
 *       [ -d <dir> ]  -> True if directory exists (stat() + S_ISDIR)
 *       [ -e <path> ] -> True if path exists (access() with F_OK)
 *       [ -s <file> ] -> True if file exists and has size > 0
 *       [ -z <str> ]  -> True if string is empty (length == 0)
 *       [ -n <str> ]  -> True if string is non-empty (length > 0)
 *     Binary:
 *       [ a = b ] / [ a == b ] -> String equality
 *       [ a != b ]             -> String inequality
 *       [ a -eq b ]            -> Numeric equal
 *       [ a -ne b ]            -> Numeric not equal
 *       [ a -gt b ]            -> Numeric greater than
 *       [ a -ge b ]            -> Numeric greater than or equal
 *       [ a -lt b ]            -> Numeric less than
 *       [ a -le b ]            -> Numeric less than or equal
 *
 * Parameters:
 *   @test_expr: Test command string, e.g. "if [ -f hello.txt ]"
 *   @ctx:       ScriptContext for variable expansion and cwd resolution.
 *
 * Returns:
 *   int: 1 if the condition evaluates to TRUE, 0 if FALSE.
 * -------------------------------------------------------------------------------------
 */
static int eval_test(const char *test_expr, ScriptContext *ctx) {
    char expanded[MAX_LINE_LEN];
    /* First expand any variables inside the test expression (e.g. [ $x -eq 5 ]) */
    expand_script_vars(ctx, test_expr, expanded, sizeof(expanded));

    /* Locate opening '[' and closing ']' brackets */
    char *start = strchr(expanded, '[');
    char *end = strrchr(expanded, ']');
    if (!start || !end || end <= start) return 0; /* Invalid brackets syntax */

    start++;        /* Move past opening '[' */
    *end = '\0';    /* Truncate at closing ']' */

    /* Tokenize arguments inside the brackets */
    char *tokens[16];
    int tcount = 0;
    char *p = start;

    while (*p && tcount < 16) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        /* Check for quoted tokens ("..." or '...') */
        if (*p == '"' || *p == '\'') {
            char q = *p++;       /* Store quote character and skip opening quote */
            tokens[tcount++] = p;
            while (*p && *p != q) p++;
            if (*p) *p++ = '\0'; /* Terminate token and advance past closing quote */
        } else {
            /* Unquoted token: advance until next space or tab */
            tokens[tcount++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0'; /* Terminate token */
        }
    }

    if (tcount == 0) return 0; /* No expressions found inside brackets */

    /* Unary tests: exactly 2 tokens, e.g. ["-f", "filename"] */
    if (tcount == 2) {
        char fullpath[MAX_PATH_LEN];
        /* Resolve relative paths against current working directory */
        if (tokens[1][0] == '/') {
            strncpy(fullpath, tokens[1], sizeof(fullpath) - 1);
            fullpath[sizeof(fullpath) - 1] = '\0';
        } else {
            snprintf(fullpath, sizeof(fullpath), "%s/%s", ctx->cwd, tokens[1]);
        }

        struct stat st; /* Struct to receive metadata from stat() */

        /* [ -f file ]: Checks if path exists and is a regular file */
        if (strcmp(tokens[0], "-f") == 0) {
            return (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
        }
        /* [ -d dir ]: Checks if path exists and is a directory */
        else if (strcmp(tokens[0], "-d") == 0) {
            return (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
        }
        /* [ -e path ]: Checks if path exists using access() with F_OK */
        else if (strcmp(tokens[0], "-e") == 0) {
            return (access(fullpath, F_OK) == 0) ? 1 : 0;
        }
        /* [ -s file ]: Checks if path exists and has a size strictly greater than 0 */
        else if (strcmp(tokens[0], "-s") == 0) {
            return (stat(fullpath, &st) == 0 && st.st_size > 0) ? 1 : 0;
        }
        /* [ -z str ]: Checks if string length is zero (empty string) */
        else if (strcmp(tokens[0], "-z") == 0) {
            return (strlen(tokens[1]) == 0) ? 1 : 0;
        }
        /* [ -n str ]: Checks if string length is non-zero */
        else if (strcmp(tokens[0], "-n") == 0) {
            return (strlen(tokens[1]) > 0) ? 1 : 0;
        }
    }

    /* Binary tests: exactly 3 tokens, e.g. ["val1", "operator", "val2"] */
    if (tcount == 3) {
        /* String equality: [ a = b ] or [ a == b ] */
        if (strcmp(tokens[1], "=") == 0 || strcmp(tokens[1], "==") == 0) {
            return (strcmp(tokens[0], tokens[2]) == 0) ? 1 : 0;
        }
        /* String inequality: [ a != b ] */
        else if (strcmp(tokens[1], "!=") == 0) {
            return (strcmp(tokens[0], tokens[2]) != 0) ? 1 : 0;
        }
        /* Numeric equality: [ a -eq b ] */
        else if (strcmp(tokens[1], "-eq") == 0) {
            return (atoi(tokens[0]) == atoi(tokens[2])) ? 1 : 0;
        }
        /* Numeric inequality: [ a -ne b ] */
        else if (strcmp(tokens[1], "-ne") == 0) {
            return (atoi(tokens[0]) != atoi(tokens[2])) ? 1 : 0;
        }
        /* Numeric greater than: [ a -gt b ] */
        else if (strcmp(tokens[1], "-gt") == 0) {
            return (atoi(tokens[0]) > atoi(tokens[2])) ? 1 : 0;
        }
        /* Numeric greater than or equal: [ a -ge b ] */
        else if (strcmp(tokens[1], "-ge") == 0) {
            return (atoi(tokens[0]) >= atoi(tokens[2])) ? 1 : 0;
        }
        /* Numeric less than: [ a -lt b ] */
        else if (strcmp(tokens[1], "-lt") == 0) {
            return (atoi(tokens[0]) < atoi(tokens[2])) ? 1 : 0;
        }
        /* Numeric less than or equal: [ a -le b ] */
        else if (strcmp(tokens[1], "-le") == 0) {
            return (atoi(tokens[0]) <= atoi(tokens[2])) ? 1 : 0;
        }
    }

    return 0; /* Default: test evaluated to FALSE */
}

/*
 * Structure: IfFrame
 * Purpose:
 *   Represents one nesting level in the conditional execution state machine.
 *   Enables proper handling of nested `if ... else ... fi` blocks.
 */
typedef struct {
    int active;        /* 1 if the parent block enclosing this 'if' is currently executing */
    int branch_taken;  /* 1 if any 'if' or 'elif' branch in this block has already evaluated to true */
    int executing;     /* 1 if the current branch of this block is actively executing lines */
} IfFrame;

/*
 * -------------------------------------------------------------------------------------
 * Function: script_execute_file
 *
 * Purpose:
 *   Executes a shell script file line-by-line in a controlled environment.
 *   Provides:
 *     - Syntax verification prior to execution.
 *     - Parameter binding ($0..$9, $#, $?).
 *     - Nested conditional branching (if/elif/else/fi).
 *     - Variable assignment (VAR=value).
 *     - Exit command handling (exit <code>).
 *     - Command execution forwarding to shell_execute_line().
 *
 * Parameters:
 *   @filepath:    Path to script file.
 *   @argc:        Number of arguments passed to script.
 *   @argv:        Array of argument strings.
 *   @out_fd:      Output file descriptor (pipe to GUI or stdout).
 *   @in_fd:       Input file descriptor.
 *   @cwd:         Initial working directory for script execution.
 *   @last_status: Pointer to integer where final exit status is written back.
 *
 * Returns:
 *   int: Exit status of the script (0 for success, non-zero for error or custom exit code).
 * -------------------------------------------------------------------------------------
 */
int script_execute_file(const char *filepath, int argc, char **argv, int out_fd, int in_fd, const char *cwd, int *last_status) {
    char err_buf[512];

    /* 1. Pre-execution static analysis: validate if/fi syntax */
    if (script_validate_syntax(filepath, err_buf, sizeof(err_buf)) != 0) {
        /* Write syntax error message to output descriptor */
        write(out_fd, err_buf, strlen(err_buf));
        if (last_status) *last_status = 2; /* Bash returns exit code 2 on syntax error */
        return 2;
    }

    /* 2. Open the script file for reading */
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        char err[512];
        snprintf(err, sizeof(err), "terminator: %s: No such file or directory\n", filepath);
        write(out_fd, err, strlen(err));
        if (last_status) *last_status = 127; /* Standard "file not found" exit code */
        return 127;
    }

    /* 3. Initialize script execution context */
    ScriptContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.last_status = 0;
    strncpy(ctx.cwd, cwd ? cwd : ".", sizeof(ctx.cwd) - 1);

    /* Copy positional arguments into script context */
    for (int i = 0; i < argc && i < 32; i++) {
        strncpy(ctx.args[i], argv[i], MAX_CMD_LEN - 1);
        ctx.arg_count++;
    }

    /* 4. Initialize conditional block execution stack */
    IfFrame if_stack[32];
    int if_top = 0;
    if_stack[0].active = 1;        /* Root level is always active */
    if_stack[0].branch_taken = 0;  /* No branch taken yet */
    if_stack[0].executing = 1;     /* Root level is executing */

    char line[MAX_LINE_LEN];

    /* Temporary dummy TabState for executing shell commands within the script */
    TabState script_tab;
    memset(&script_tab, 0, sizeof(TabState));
    strncpy(script_tab.cwd, ctx.cwd, sizeof(script_tab.cwd) - 1);
    script_tab.last_status = 0;
    job_list_init(&script_tab.jobs); /* Initialize job control list */

    /* 5. Main interpretation loop: read line-by-line */
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newlines ('\n' or '\r') */
        size_t slen = strlen(line);
        while (slen > 0 && (line[slen - 1] == '\n' || line[slen - 1] == '\r')) {
            line[--slen] = '\0';
        }

        /* Skip leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue; /* Skip comments and blank lines */

        int is_executing = if_stack[if_top].executing;

        /* Condition 1: Check for 'if' block */
        if (strncmp(p, "if ", 3) == 0 || strncmp(p, "if\t", 3) == 0) {
            int parent_exec = if_stack[if_top].executing;
            if_top++; /* Push new frame onto stack */
            if_stack[if_top].active = parent_exec;
            if (parent_exec) {
                int test_res = eval_test(p, &ctx);
                if_stack[if_top].branch_taken = test_res;
                if_stack[if_top].executing = test_res;
            } else {
                if_stack[if_top].branch_taken = 1;
                if_stack[if_top].executing = 0;
            }
            continue;
        }

        /* Condition 2: Check for 'elif' block */
        if (strncmp(p, "elif ", 5) == 0 || strncmp(p, "elif\t", 5) == 0) {
            /* Execute elif only if parent is active and no previous branch was taken */
            if (if_stack[if_top].active && !if_stack[if_top].branch_taken) {
                int test_res = eval_test(p, &ctx);
                if_stack[if_top].branch_taken = test_res;
                if_stack[if_top].executing = test_res;
            } else {
                if_stack[if_top].executing = 0;
            }
            continue;
        }

        /* Condition 3: Check for 'else' block */
        if (strncmp(p, "else", 4) == 0 && (p[4] == ' ' || p[4] == '\t' || p[4] == '\0' || p[4] == ';')) {
            /* Execute else only if parent is active and no previous branch was taken */
            if (if_stack[if_top].active && !if_stack[if_top].branch_taken) {
                if_stack[if_top].branch_taken = 1;
                if_stack[if_top].executing = 1;
            } else {
                if_stack[if_top].executing = 0;
            }
            continue;
        }

        /* Condition 4: Check for 'fi' block */
        if (strncmp(p, "fi", 2) == 0 && (p[2] == ' ' || p[2] == '\t' || p[2] == '\0' || p[2] == ';')) {
            if (if_top > 0) if_top--; /* Pop top frame from stack */
            continue;
        }

        /* Skip stand-alone 'then' keyword */
        if (strncmp(p, "then", 4) == 0) {
            continue;
        }

        /* If current conditional branch is disabled (not executing), skip the line */
        if (!is_executing) {
            continue;
        }

        /* Variable assignment check: NAME=value (without whitespace around '=') */
        char *eq = strchr(p, '=');
        if (eq && eq > p && !strchr(p, ' ') && !strchr(p, '\t')) {
            *eq = '\0';                /* Separate variable name from value */
            char *vname = p;
            char *vval = eq + 1;
            char exp_val[MAX_CMD_LEN];
            /* Expand any nested variables in the assigned value */
            expand_script_vars(&ctx, vval, exp_val, sizeof(exp_val));
            set_var(&ctx, vname, exp_val);
            continue;
        }

        /* Built-in 'exit' command check within script */
        if (strncmp(p, "exit", 4) == 0 && (p[4] == ' ' || p[4] == '\0' || p[4] == '\t')) {
            int code = 0;
            if (p[4] != '\0') {
                code = atoi(p + 5); /* Parse optional integer exit code */
            }
            ctx.last_status = code;
            break; /* Terminate script loop early */
        }

        /* Normal command line execution */
        char expanded_line[MAX_CMD_LEN];
        /* Expand all variables in the command line */
        expand_script_vars(&ctx, p, expanded_line, sizeof(expanded_line));

        /* Execute command using the main Terminator shell pipeline dispatcher */
        int res = shell_execute_line(&script_tab, expanded_line, out_fd, in_fd);
        ctx.last_status = res; /* Record execution status for subsequent $? checks */
    }

    fclose(fp); /* Close script file */
    if (last_status) *last_status = ctx.last_status; /* Pass back final status */
    return ctx.last_status; /* Return exit status to caller */
}
