/*
 * =====================================================================================
 * File: src/shell.c
 * Project: Terminator - Custom Tabbed Terminal Emulator & Shell
 *
 * Educational Purpose & Viva Guide:
 * This file represents the core execution engine of the Terminator shell. Each tab
 * runs an instance of this shell worker (either as a POSIX thread or a dedicated child process).
 * It implements:
 *   1. Full Line-Editing Engine:
 *      - Real-time character insertion, backspace, and delete using memmove().
 *      - Cursor movement: Left/Right, Home (Ctrl+A), End (Ctrl+E).
 *      - Emacs-style kill ring shortcuts: Delete to start (Ctrl+U), Delete to end (Ctrl+K),
 *        Delete word (Ctrl+W), Clear screen (Ctrl+L).
 *      - History browsing: Up/Down arrows navigating POSIX shared memory history.
 *      - Interactive Reverse History Search: Ctrl+R mode with dynamic string matching.
 *      - Tab auto-completion: single match completion, longest common prefix expansion,
 *        and numbered choice selection for ambiguous prefixes.
 *      - Multiline command accumulation: tracks incomplete quotes and trailing backslashes.
 *   2. Command & Pipeline Execution Engine:
 *      - Variable expansion ($VAR, $?, $#, $0..$9) and command substitution ($(cmd) and `cmd`).
 *      - Direct built-in command dispatching without process spawning (cd, pwd, jobs, fg, bg, etc.).
 *      - Bash script (.sh) execution dispatching.
 *      - Multi-stage pipeline execution: pipe() creation, fork(), process grouping (setpgid),
 *        file descriptor redirection with dup2() (for <, >, >>), and execvp().
 *      - Process synchronization and Job Control: waitpid() with WUNTRACED, signal handling
 *        (Ctrl+C SIGINT, Ctrl+Z SIGTSTP), and exit status extraction (WIFEXITED, WEXITSTATUS).
 *   3. Tab Concurrency & IPC:
 *      - Thread Mode (pthread_create + pthread_join) and Process Mode (fork + waitpid).
 *      - Full-duplex pipe-based IPC messaging (IpcMessage) passing key events and signals.
 *
 * System Calls & Libraries Used:
 *   - <unistd.h>: fork(), execvp(), pipe(), dup2(), close(), read(), write(), chdir(), getcwd(), access().
 *   - <sys/wait.h>: waitpid(), WIFEXITED(), WEXITSTATUS(), WIFSIGNALED(), WTERMSIG(), WIFSTOPPED().
 *   - <fcntl.h>: open(), O_RDONLY, O_WRONLY, O_CREAT, O_APPEND, O_TRUNC for I/O redirection.
 *   - <signal.h>: kill(), SIGINT, SIGTSTP, SIGTERM for process and job control signaling.
 *   - <pthread.h>: pthread_create(), pthread_join() for multithreaded tab workers.
 *   - <sys/stat.h>: stat(), S_ISREG() for script file inspection.
 * =====================================================================================
 */

#include "shell.h"       /* Header declaring TabState, shell_tab_init(), shell_execute_line(), etc. */
#include "parser.h"      /* Header for pipeline parsing and parameter expansion */
#include "builtins.h"    /* Header for shell built-in command dispatching */
#include "history.h"     /* Header for POSIX shared memory command history */
#include "completion.h"  /* Header for tab auto-completion primitives */
#include "script.h"      /* Header for custom bash script execution */

/*
 * -------------------------------------------------------------------------------------
 * Function: print_prompt (static helper)
 *
 * Purpose:
 *   Outputs the shell command prompt to the tab's output file descriptor.
 *   If the shell is currently accumulating a multiline command (e.g. open quote),
 *   it prints the continuation prompt ("> ").
 *   Otherwise, it prints the standard prompt ("user@terminator> ").
 *
 * Parameters:
 *   @tab:    Pointer to the TabState.
 *   @out_fd: Output file descriptor (pipe to GUI).
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static void print_prompt(TabState *tab, int out_fd) {
    if (tab->in_multiline) {
        const char *p = "> ";
        term_write(out_fd, p, strlen(p));
    } else {
        const char *p = "user@terminator> ";
        term_write(out_fd, p, strlen(p));
    }
}

/*
 * -------------------------------------------------------------------------------------
 * Function: redraw_current_line (static helper)
 *
 * Purpose:
 *   Redraws the active command line being edited by the user.
 *   Uses ANSI escape sequences:
 *     - '\r': Moves cursor to column 0.
 *     - Prompt + input string: Prints the latest line content.
 *     - "\033[K": Clears from cursor position to the end of the line (clearing old trailing chars).
 *     - "\033[<N>D": Moves cursor backward N columns if the cursor is positioned
 *       in the middle of the string rather than at the end.
 *
 * Parameters:
 *   @tab:    Pointer to TabState.
 *   @out_fd: Output file descriptor.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static void redraw_current_line(TabState *tab, int out_fd) {
    char buf[MAX_CMD_LEN * 2];
    const char *prompt = tab->in_multiline ? "> " : "user@terminator> ";

    /* \r moves cursor to start of line; \033[K erases everything to the right */
    snprintf(buf, sizeof(buf), "\r%s%s\033[K", prompt, tab->input_line);
    term_write(out_fd, buf, strlen(buf));

    /* If cursor is not at the end of input_line, move cursor back to cursor_pos */
    int len = strlen(tab->input_line);
    if (tab->cursor_pos < len) {
        int back = len - tab->cursor_pos;
        char back_buf[64];
        /* ANSI escape \033[<N>D moves cursor backward by N cells */
        snprintf(back_buf, sizeof(back_buf), "\033[%dD", back);
        term_write(out_fd, back_buf, strlen(back_buf));
    }
}

/*
 * -------------------------------------------------------------------------------------
 * Function: shell_tab_init
 *
 * Purpose:
 *   Initializes a TabState structure to a clean, well-defined starting state:
 *     - Sets tab ID and execution mode (thread vs process).
 *     - Initializes file descriptors to -1 (unconnected).
 *     - Sets initial working directory (prefers $HOME, falls back to getcwd()).
 *     - Initializes job control list for background job tracking.
 *
 * Parameters:
 *   @tab:  Pointer to TabState structure to initialize.
 *   @id:   Unique positive integer identifier assigned to this tab.
 *   @mode: ExecutionMode enum (MODE_THREAD or MODE_PROCESS).
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
void shell_tab_init(TabState *tab, int id, ExecutionMode mode) {
    if (!tab) return;
    memset(tab, 0, sizeof(TabState)); /* Zero all fields */
    tab->id = id;
    tab->mode = mode;
    tab->is_active = (id == 1);       /* Tab 1 starts as active */
    tab->last_status = 0;             /* Last exit status initially 0 */
    tab->cursor_pos = 0;              /* Cursor at position 0 */
    tab->to_tab_fd = -1;              /* Not yet connected */
    tab->from_tab_fd = -1;
    tab->tab_in_fd = -1;
    tab->tab_out_fd = -1;
    tab->hist_browse_idx = -1;        /* Not currently browsing history */

    /* Set default working directory to $HOME or current process directory */
    const char *home = getenv("HOME");
    if (home) {
        strncpy(tab->cwd, home, sizeof(tab->cwd) - 1);
    } else {
        getcwd(tab->cwd, sizeof(tab->cwd) - 1);
    }
    tab->old_pwd[0] = '\0'; /* Initialize previous directory for 'cd -' */

    /* Initialize job control tracking list */
    job_list_init(&tab->jobs);
}

/*
 * -------------------------------------------------------------------------------------
 * Function: shell_execute_line
 *
 * Purpose:
 *   The primary command execution coordinator of the Terminator shell:
 *     1. Performs lexical expansion: variable substitution ($VAR, $?, $#) and command
 *        substitutions ($(cmd) and `cmd`) via parser_expand().
 *     2. Parses command pipelines into discrete commands with input/output redirections
 *        via parser_parse_pipeline().
 *     3. Single Built-in Optimization: If the command is a single built-in (cd, pwd, etc.)
 *        with no pipe, executes it directly in the shell process to preserve state changes
 *        (like changing directories with chdir).
 *     4. Script (.sh) Execution: If the command targets a bash script, invokes script_execute_file().
 *     5. Redirection Validation: Confirms input redirection files (< infile) exist before
 *        forking processes.
 *     6. Pipeline Execution:
 *        - Creates UNIX pipes between adjacent commands using pipe().
 *        - Spawns child processes using fork().
 *        - Configures process groups via setpgid() to isolate terminal job control.
 *        - Duplicates file descriptors via dup2() to wire pipes and file redirections (<, >, >>).
 *        - Executes binary via execvp().
 *     7. Foreground/Background Synchronization:
 *        - Background (&): Adds job to job list and prints [jid] pid without blocking.
 *        - Foreground: Sets fg_pgid and waits on child processes with waitpid(..., WUNTRACED).
 *        - Evaluates termination status using WIFEXITED, WEXITSTATUS, WIFSIGNALED, WIFSTOPPED.
 *
 * Parameters:
 *   @tab:      Pointer to TabState.
 *   @raw_line: Raw input string entered by user.
 *   @out_fd:   File descriptor where command output should be routed.
 *   @in_fd:    File descriptor for command input.
 *
 * Returns:
 *   int: Exit status of the executed command (0 for success, non-zero for error).
 * -------------------------------------------------------------------------------------
 */
int shell_execute_line(TabState *tab, const char *raw_line, int out_fd, int in_fd) {
    if (!raw_line || strlen(raw_line) == 0) return 0;

    /* 1. Expand variables ($VAR, $?), command substitutions $(...), and backticks */
    char *expanded = parser_expand(raw_line, tab->last_status, tab->cwd);
    if (!expanded) expanded = strdup(raw_line);

    Pipeline pl;
    /* 2. Parse command string into a Pipeline struct containing commands, arguments, redirections */
    if (parser_parse_pipeline(expanded, &pl) != 0 || pl.cmd_count == 0) {
        free(expanded);
        return 0;
    }

    /* 3. Check for single built-in command without pipe (e.g. "cd", "export", "exit") */
    if (pl.cmd_count == 1 && builtin_is_builtin(pl.commands[0].args[0])) {
        int ret = builtin_execute(tab, pl.commands[0].args, pl.commands[0].arg_count, out_fd, in_fd);
        tab->last_status = ret;
        pipeline_free(&pl);
        free(expanded);
        return ret;
    }

    /* 4. Check for direct execution of bash script (.sh file or shebang) */
    if (pl.cmd_count == 1 && pl.commands[0].arg_count > 0) {
        const char *cmd0 = pl.commands[0].args[0];
        char script_path[MAX_PATH_LEN] = {0};

        /* Resolve relative or absolute path */
        if (cmd0[0] == '/') {
            strncpy(script_path, cmd0, sizeof(script_path) - 1);
        } else if (strncmp(cmd0, "./", 2) == 0 || strncmp(cmd0, "../", 3) == 0) {
            snprintf(script_path, sizeof(script_path), "%s/%s", tab->cwd, cmd0);
        } else if (strstr(cmd0, ".sh")) {
            snprintf(script_path, sizeof(script_path), "%s/%s", tab->cwd, cmd0);
        }

        if (strlen(script_path) > 0) {
            struct stat st;
            /* Verify if path exists and is a regular file */
            if (stat(script_path, &st) == 0 && S_ISREG(st.st_mode)) {
                FILE *sfp = fopen(script_path, "r");
                if (sfp) {
                    char first_line[128] = {0};
                    fgets(first_line, sizeof(first_line), sfp);
                    fclose(sfp);
                    /* If script contains bash shebang or ends in .sh, run with script interpreter */
                    if (strstr(first_line, "bash") || strstr(first_line, "sh") || strstr(cmd0, ".sh")) {
                        int status = 0;
                        int res = script_execute_file(script_path, pl.commands[0].arg_count,
                                                      pl.commands[0].args, out_fd, in_fd, tab->cwd, &status);
                        tab->last_status = status;
                        pipeline_free(&pl);
                        free(expanded);
                        return res;
                    }
                }
            }
        }
    }

    /* 5. Pre-execution check: verify all input redirection files (< infile) exist */
    for (int i = 0; i < pl.cmd_count; i++) {
        if (pl.commands[i].infile) {
            char in_path[MAX_PATH_LEN];
            if (pl.commands[i].infile[0] == '/') {
                strncpy(in_path, pl.commands[i].infile, sizeof(in_path) - 1);
            } else {
                snprintf(in_path, sizeof(in_path), "%s/%s", tab->cwd, pl.commands[i].infile);
            }
            /* access() with R_OK verifies file existence and read permissions */
            if (access(in_path, R_OK) != 0) {
                char err[512];
                snprintf(err, sizeof(err), "terminator: %s: No such file or directory\n", pl.commands[i].infile);
                term_write(out_fd, err, strlen(err));
                tab->last_status = 1;
                pipeline_free(&pl);
                free(expanded);
                return 1;
            }
        }
    }

    /* 6. Execute external command pipeline */
    int num_cmds = pl.cmd_count;
    int pipe_fds[MAX_PIPES][2]; /* Array of file descriptor pairs for inter-command pipes */

    /* Create (num_cmds - 1) pipes connecting adjacent commands */
    for (int i = 0; i < num_cmds - 1; i++) {
        if (pipe(pipe_fds[i]) < 0) {
            perror("pipe creation failed");
            pipeline_free(&pl);
            free(expanded);
            return 1;
        }
    }

    pid_t pids[MAX_PIPES]; /* Store child process IDs */
    pid_t first_pid = 0;   /* Process Group Leader ID */

    /* Fork and execute each command in the pipeline */
    for (int i = 0; i < num_cmds; i++) {
        SimpleCommand *sc = &pl.commands[i];
        pids[i] = fork(); /* Create child process */

        if (pids[i] == 0) {
            /* ------------------ CHILD PROCESS ------------------ */

            /* Process Grouping: Set process group ID to isolate pipeline from GUI */
            if (i == 0) {
                setpgid(0, 0);         /* First child becomes process group leader */
            } else {
                setpgid(0, first_pid); /* Subsequent children join the first child's group */
            }

            /* Apply current working directory of this tab */
            if (tab->cwd && strlen(tab->cwd) > 0) {
                chdir(tab->cwd);
            }

            /* Configure Input Redirection */
            if (sc->infile) {
                /* Explicit input file: < infile */
                int ifd = open(sc->infile, O_RDONLY);
                if (ifd >= 0) {
                    dup2(ifd, STDIN_FILENO); /* Redirect standard input to file */
                    close(ifd);
                }
            } else if (i > 0) {
                /* Pipe input from previous command */
                dup2(pipe_fds[i - 1][0], STDIN_FILENO);
            }

            /* Configure Output Redirection */
            if (sc->outfile) {
                /* Explicit output file: > outfile (truncate) or >> outfile (append) */
                int flags = O_WRONLY | O_CREAT | (sc->append_out ? O_APPEND : O_TRUNC);
                int ofd = open(sc->outfile, flags, 0644);
                if (ofd >= 0) {
                    dup2(ofd, STDOUT_FILENO); /* Redirect standard output to file */
                    close(ofd);
                }
            } else if (i < num_cmds - 1) {
                /* Pipe output to next command */
                dup2(pipe_fds[i][1], STDOUT_FILENO);
            } else {
                /* Last command in pipeline sends output to out_fd (GUI pipe) */
                dup2(out_fd, STDOUT_FILENO);
            }

            /* Redirect standard error (stderr) to out_fd so error messages appear in GUI */
            dup2(out_fd, STDERR_FILENO);

            /* Close all inter-command pipe descriptors in the child process */
            for (int k = 0; k < num_cmds - 1; k++) {
                close(pipe_fds[k][0]);
                close(pipe_fds[k][1]);
            }

            /* execvp(): Replace child process image with the target executable */
            execvp(sc->args[0], sc->args);

            /* If execvp() returns, the command failed to execute (e.g. command not found) */
            char err[512];
            snprintf(err, sizeof(err), "terminator: %s: command not found\n", sc->args[0]);
            term_write(STDERR_FILENO, err, strlen(err));
            exit(127); /* Standard exit code for "command not found" */
        } else if (pids[i] > 0) {
            /* ------------------ PARENT PROCESS ------------------ */
            if (i == 0) {
                first_pid = pids[0]; /* Record first process PID as PGID */
            }
            setpgid(pids[i], first_pid); /* Ensure parent also sets pgid to avoid race condition */
        } else {
            perror("fork failed");
        }
    }

    /* Parent: Close all pipe descriptors so EOF signals propagate through pipeline */
    for (int k = 0; k < num_cmds - 1; k++) {
        close(pipe_fds[k][0]);
        close(pipe_fds[k][1]);
    }

    /* 7. Handle Background Execution vs Foreground Synchronization */
    if (pl.is_background) {
        /* Background job (&): do not wait; register in jobs list */
        int jid = job_add(&tab->jobs, first_pid, raw_line, JOB_RUNNING);
        char bg_msg[128];
        snprintf(bg_msg, sizeof(bg_msg), "[%d] %d\n", jid, first_pid);
        term_write(out_fd, bg_msg, strlen(bg_msg));
        tab->last_status = 0;
    } else {
        /* Foreground job: wait for all children to complete or stop */
        tab->jobs.fg_pgid = first_pid;
        strncpy(tab->jobs.fg_cmd, raw_line, sizeof(tab->jobs.fg_cmd) - 1);
        tab->is_busy = 1;

        int status;
        for (int i = 0; i < num_cmds; i++) {
            /* waitpid() with WUNTRACED reports stopped children (Ctrl+Z SIGTSTP) */
            pid_t wpid = waitpid(pids[i], &status, WUNTRACED);
            if (wpid > 0 && WIFSTOPPED(status)) {
                /* Process was suspended via Ctrl+Z */
                int jid = job_add(&tab->jobs, first_pid, raw_line, JOB_STOPPED);
                char stop_msg[512];
                snprintf(stop_msg, sizeof(stop_msg), "\n[%d]+ Stopped %s\n", jid, raw_line);
                term_write(out_fd, stop_msg, strlen(stop_msg));
                break;
            } else if (wpid > 0 && i == num_cmds - 1) {
                /* Exit status of the LAST command in the pipeline determines overall status */
                if (WIFEXITED(status)) {
                    tab->last_status = WEXITSTATUS(status);        /* Normal exit: extract return code */
                } else if (WIFSIGNALED(status)) {
                    tab->last_status = 128 + WTERMSIG(status);     /* Terminated by signal: 128 + signal */
                }
            }
        }

        tab->jobs.fg_pgid = 0; /* Clear active foreground process group */
        tab->is_busy = 0;
    }

    pipeline_free(&pl); /* Free dynamically allocated pipeline memory */
    free(expanded);     /* Free expanded command string */
    return tab->last_status;
}

/*
 * -------------------------------------------------------------------------------------
 * Function: handle_key_event (static helper)
 *
 * Purpose:
 *   Handles incoming user keystrokes for the active tab:
 *     1. Completion Choice State: User presses digit 1..9 to pick a completion match.
 *     2. Ctrl+R Mode: Real-time dynamic string matching across command history.
 *     3. Line-Editing Actions:
 *        - Character insertion into input buffer with cursor tracking.
 *        - Cursor movement (Left, Right, Home/Ctrl+A, End/Ctrl+E).
 *        - Line deletions (Ctrl+U, Ctrl+K, Ctrl+W, Backspace, Delete).
 *        - Screen clearing (Ctrl+L sends ANSI "\033[2J\033[H").
 *        - History browsing (Up arrow for older commands, Down arrow for newer).
 *        - Tab Auto-completion: Dispatches complete_word() for files and commands.
 *        - Ctrl+D (EOF): Closes tab if input line is empty.
 *        - Enter: Appends line to multiline accumulator; if complete, sends to history
 *          and executes via shell_execute_line().
 *
 * Parameters:
 *   @tab:    Pointer to TabState.
 *   @ev:     Pointer to TermKeyEvent received from GUI.
 *   @out_fd: Output file descriptor.
 *   @in_fd:  Input file descriptor.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static void handle_key_event(TabState *tab, const TermKeyEvent *ev, int out_fd, int in_fd) {
    if (!tab || !ev) return;

    /* 1. If currently waiting for user to select from numbered completion list (1..9) */
    if (tab->waiting_completion_choice) {
        if (ev->code == KEY_CODE_CHAR && isdigit((unsigned char)ev->ch)) {
            int choice = ev->ch - '1'; /* Convert ASCII '1'..'9' to 0-based index 0..8 */
            if (choice >= 0 && choice < tab->completion_res.match_count) {
                const char *selected = tab->completion_res.matches[choice];

                /* Replace current incomplete word with selected filename */
                int wstart = tab->completion_res.word_start;
                int dir_len = strlen(tab->completion_res.dir_part);
                int repl_start = wstart + dir_len;

                char new_line[MAX_CMD_LEN] = {0};
                strncpy(new_line, tab->input_line, repl_start);
                strcat(new_line, selected);
                int sel_len = strlen(new_line);
                strcat(new_line, tab->input_line + tab->completion_res.word_end);

                strncpy(tab->input_line, new_line, sizeof(tab->input_line) - 1);
                tab->cursor_pos = sel_len;
            }
        }
        tab->waiting_completion_choice = 0; /* Reset completion choice mode */
        redraw_current_line(tab, out_fd);
        return;
    }

    /* 2. If in interactive Ctrl+R history search mode */
    if (tab->in_ctrl_r) {
        if (ev->code == KEY_CODE_ENTER) {
            term_write(out_fd, "\n", 1);
            char res_buf[4096];
            /* Perform Longest Common Subsequence / substring search in shared history */
            history_search_ctrl_r(tab->search_term, res_buf, sizeof(res_buf));
            term_write(out_fd, res_buf, strlen(res_buf));

            tab->in_ctrl_r = 0;
            tab->search_term[0] = '\0';
            tab->input_line[0] = '\0';
            tab->cursor_pos = 0;
            print_prompt(tab, out_fd);
        } else if (ev->code == KEY_CODE_BACKSPACE) {
            int slen = strlen(tab->search_term);
            if (slen > 0) {
                tab->search_term[slen - 1] = '\0'; /* Remove last character of search query */
                char pbuf[512];
                snprintf(pbuf, sizeof(pbuf), "\rEnter search term: %s\033[K", tab->search_term);
                term_write(out_fd, pbuf, strlen(pbuf));
            }
        } else if (ev->code == KEY_CODE_CTRL_C) {
            tab->in_ctrl_r = 0;
            tab->search_term[0] = '\0';
            term_write(out_fd, "^C\n", 3);
            print_prompt(tab, out_fd);
        } else if (ev->code == KEY_CODE_CHAR && isprint((unsigned char)ev->ch)) {
            int slen = strlen(tab->search_term);
            if (slen < MAX_CMD_LEN - 2) {
                tab->search_term[slen] = ev->ch; /* Append typed char to search query */
                tab->search_term[slen + 1] = '\0';
                term_write(out_fd, &ev->ch, 1);
            }
        }
        return;
    }

    /* 3. Normal command line editing */
    switch (ev->code) {
        case KEY_CODE_CHAR: {
            int len = strlen(tab->input_line);
            if (len < MAX_CMD_LEN - 2) {
                /* Shift characters to the right to make room for inserted character */
                memmove(tab->input_line + tab->cursor_pos + 1,
                        tab->input_line + tab->cursor_pos,
                        len - tab->cursor_pos + 1);
                tab->input_line[tab->cursor_pos] = ev->ch;
                tab->cursor_pos++;
                redraw_current_line(tab, out_fd);
            }
            break;
        }
        case KEY_CODE_LEFT: {
            if (tab->cursor_pos > 0) {
                tab->cursor_pos--; /* Move cursor one position left */
                redraw_current_line(tab, out_fd);
            }
            break;
        }
        case KEY_CODE_RIGHT: {
            if (tab->cursor_pos < (int)strlen(tab->input_line)) {
                tab->cursor_pos++; /* Move cursor one position right */
                redraw_current_line(tab, out_fd);
            }
            break;
        }
        case KEY_CODE_CTRL_A: {
            tab->cursor_pos = 0; /* Move cursor to line beginning (Home) */
            redraw_current_line(tab, out_fd);
            break;
        }
        case KEY_CODE_CTRL_E: {
            tab->cursor_pos = strlen(tab->input_line); /* Move cursor to line end (End) */
            redraw_current_line(tab, out_fd);
            break;
        }
        case KEY_CODE_CTRL_U: {
            /* Delete text from line beginning up to current cursor position */
            int len = strlen(tab->input_line);
            memmove(tab->input_line, tab->input_line + tab->cursor_pos, len - tab->cursor_pos + 1);
            tab->cursor_pos = 0;
            redraw_current_line(tab, out_fd);
            break;
        }
        case KEY_CODE_CTRL_K: {
            /* Delete text from cursor position to line end */
            tab->input_line[tab->cursor_pos] = '\0';
            redraw_current_line(tab, out_fd);
            break;
        }
        case KEY_CODE_CTRL_W: {
            /* Delete word immediately preceding the cursor */
            if (tab->cursor_pos > 0) {
                int end = tab->cursor_pos;
                int start = end;
                while (start > 0 && tab->input_line[start - 1] == ' ') start--;
                while (start > 0 && tab->input_line[start - 1] != ' ') start--;
                int len = strlen(tab->input_line);
                memmove(tab->input_line + start, tab->input_line + end, len - end + 1);
                tab->cursor_pos = start;
                redraw_current_line(tab, out_fd);
            }
            break;
        }
        case KEY_CODE_CTRL_L: {
            /* Clear screen: \033[2J clears display; \033[H repositions cursor at top-left */
            term_write(out_fd, "\033[2J\033[H", 7);
            redraw_current_line(tab, out_fd);
            break;
        }
        case KEY_CODE_BACKSPACE: {
            /* Delete character immediately preceding the cursor */
            if (tab->cursor_pos > 0) {
                int len = strlen(tab->input_line);
                memmove(tab->input_line + tab->cursor_pos - 1,
                        tab->input_line + tab->cursor_pos,
                        len - tab->cursor_pos + 1);
                tab->cursor_pos--;
                redraw_current_line(tab, out_fd);
            }
            break;
        }
        case KEY_CODE_DELETE: {
            /* Delete character directly under the cursor */
            int len = strlen(tab->input_line);
            if (tab->cursor_pos < len) {
                memmove(tab->input_line + tab->cursor_pos,
                        tab->input_line + tab->cursor_pos + 1,
                        len - tab->cursor_pos);
                redraw_current_line(tab, out_fd);
            }
            break;
        }
        case KEY_CODE_UP: {
            /* Browse history backward (older commands) */
            int hcount = history_count();
            if (hcount > 0) {
                if (tab->hist_browse_idx == -1) {
                    /* Save user's currently typed unsubmitted input */
                    strncpy(tab->saved_input, tab->input_line, sizeof(tab->saved_input) - 1);
                    tab->hist_browse_idx = hcount - 1;
                } else if (tab->hist_browse_idx > 0) {
                    tab->hist_browse_idx--;
                }
                char hcmd[MAX_CMD_LEN];
                if (history_get(tab->hist_browse_idx, hcmd, sizeof(hcmd))) {
                    strncpy(tab->input_line, hcmd, sizeof(tab->input_line) - 1);
                    tab->cursor_pos = strlen(tab->input_line);
                    redraw_current_line(tab, out_fd);
                }
            }
            break;
        }
        case KEY_CODE_DOWN: {
            /* Browse history forward (newer commands) */
            int hcount = history_count();
            if (tab->hist_browse_idx != -1) {
                if (tab->hist_browse_idx < hcount - 1) {
                    tab->hist_browse_idx++;
                    char hcmd[MAX_CMD_LEN];
                    if (history_get(tab->hist_browse_idx, hcmd, sizeof(hcmd))) {
                        strncpy(tab->input_line, hcmd, sizeof(tab->input_line) - 1);
                        tab->cursor_pos = strlen(tab->input_line);
                        redraw_current_line(tab, out_fd);
                    }
                } else {
                    /* Restored user's original unsubmitted input */
                    tab->hist_browse_idx = -1;
                    strncpy(tab->input_line, tab->saved_input, sizeof(tab->input_line) - 1);
                    tab->cursor_pos = strlen(tab->input_line);
                    redraw_current_line(tab, out_fd);
                }
            }
            break;
        }
        case KEY_CODE_TAB: {
            /* Tab Auto-Completion */
            int matches = complete_word(tab->input_line, tab->cursor_pos, tab->cwd, &tab->completion_res);
            if (matches == 1) {
                /* Exactly one match: automatically complete word in input line */
                int wstart = tab->completion_res.word_start;
                int dir_len = strlen(tab->completion_res.dir_part);
                int repl_start = wstart + dir_len;

                char new_line[MAX_CMD_LEN] = {0};
                strncpy(new_line, tab->input_line, repl_start);
                strcat(new_line, tab->completion_res.common_prefix);
                int sel_len = strlen(new_line);
                strcat(new_line, tab->input_line + tab->completion_res.word_end);

                strncpy(tab->input_line, new_line, sizeof(tab->input_line) - 1);
                tab->cursor_pos = sel_len;
                redraw_current_line(tab, out_fd);
            } else if (matches > 1) {
                /* Multiple matches: extend to Longest Common Prefix (LCP) */
                int wstart = tab->completion_res.word_start;
                int dir_len = strlen(tab->completion_res.dir_part);
                int repl_start = wstart + dir_len;

                if (strlen(tab->completion_res.common_prefix) > 0) {
                    char new_line[MAX_CMD_LEN] = {0};
                    strncpy(new_line, tab->input_line, repl_start);
                    strcat(new_line, tab->completion_res.common_prefix);
                    int sel_len = strlen(new_line);
                    strcat(new_line, tab->input_line + tab->completion_res.word_end);

                    strncpy(tab->input_line, new_line, sizeof(tab->input_line) - 1);
                    tab->cursor_pos = sel_len;
                }

                /* Display numbered list (1..9) of possible completions */
                char list_buf[4096];
                int offset = 0;
                term_write(out_fd, "\n", 1);
                for (int m = 0; m < tab->completion_res.match_count && m < 9; m++) {
                    offset += snprintf(list_buf + offset, sizeof(list_buf) - offset,
                                       "%d. %s  ", m + 1, tab->completion_res.matches[m]);
                }
                list_buf[offset++] = '\n';
                list_buf[offset] = '\0';
                term_write(out_fd, list_buf, strlen(list_buf));

                tab->waiting_completion_choice = 1; /* Wait for user to select 1..9 */
                print_prompt(tab, out_fd);
                redraw_current_line(tab, out_fd);
            }
            break;
        }
        case KEY_CODE_CTRL_R: {
            /* Enter Ctrl+R reverse interactive history search mode */
            tab->in_ctrl_r = 1;
            tab->search_term[0] = '\0';
            const char *cr_prompt = "(Ctrl+R)\nEnter search term: ";
            term_write(out_fd, "\n", 1);
            term_write(out_fd, cr_prompt, strlen(cr_prompt));
            break;
        }
        case KEY_CODE_CTRL_D: {
            /* Ctrl+D on empty line sends EOF to exit shell */
            if (strlen(tab->input_line) == 0) {
                tab->tab_exited = 1; /* Mark tab as exited */
            }
            break;
        }
        case KEY_CODE_ENTER: {
            term_write(out_fd, "\n", 1); /* Echo newline */

            /* Accumulate input line for multiline command handling */
            if (tab->in_multiline) {
                strcat(tab->multiline_accum, "\n");
                strcat(tab->multiline_accum, tab->input_line);
            } else {
                strncpy(tab->multiline_accum, tab->input_line, sizeof(tab->multiline_accum) - 1);
            }

            /* Check if command line is syntactically incomplete (e.g. unclosed quote) */
            if (parser_is_incomplete(tab->multiline_accum)) {
                tab->in_multiline = 1;
                tab->input_line[0] = '\0';
                tab->cursor_pos = 0;
                print_prompt(tab, out_fd); /* Display continuation prompt "> " */
            } else {
                tab->in_multiline = 0;
                tab->hist_browse_idx = -1;

                /* If accumulated command is non-empty, save to history and execute */
                if (strlen(tab->multiline_accum) > 0) {
                    history_add(tab->multiline_accum);
                    shell_execute_line(tab, tab->multiline_accum, out_fd, in_fd);
                }

                /* If command was 'exit', abort prompt reprint */
                if (tab->tab_exited) {
                    break;
                }

                /* Clear input buffers and reprint prompt */
                tab->multiline_accum[0] = '\0';
                tab->input_line[0] = '\0';
                tab->cursor_pos = 0;
                print_prompt(tab, out_fd);
            }
            break;
        }
        default:
            break;
    }
}

/*
 * -------------------------------------------------------------------------------------
 * Function: shell_worker_loop (static helper)
 *
 * Purpose:
 *   The infinite event loop run by each tab worker (thread or child process):
 *     1. Prints initial shell prompt.
 *     2. Reads IpcMessage packets from in_fd (sent by GUI):
 *        - MSG_TYPE_KEY: Dispatches key events to handle_key_event().
 *        - MSG_TYPE_SIGNAL: Dispatches SIGINT (Ctrl+C) or SIGTSTP (Ctrl+Z)
 *          to the active foreground process group.
 *        - MSG_TYPE_EXIT: Breaks the loop cleanly.
 *     3. On loop exit (e.g. user typed 'exit' or tab closed), closes tab_out_fd
 *        and tab_in_fd so the GUI detects EOF on read() and cleans up the tab.
 *
 * Parameters:
 *   @tab:    Pointer to TabState.
 *   @out_fd: Pipe descriptor to send output to GUI.
 *   @in_fd:  Pipe descriptor to receive key/signal messages from GUI.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static void shell_worker_loop(TabState *tab, int out_fd, int in_fd) {
    print_prompt(tab, out_fd); /* Print initial "user@terminator> " prompt */

    while (!tab->tab_exited) {
        IpcMessage msg;
        /* read() blocks until GUI sends an IPC message */
        ssize_t n = read(in_fd, &msg, sizeof(msg));
        if (n <= 0) {
            break; /* Pipe was closed by GUI */
        }

        /* Message 1: Keystroke event */
        if (msg.type == MSG_TYPE_KEY) {
            TermKeyEvent *ev = (TermKeyEvent *)msg.data;
            handle_key_event(tab, ev, out_fd, in_fd);
            if (tab->tab_exited) {
                break;
            }
        }
        /* Message 2: POSIX Signal (Ctrl+C or Ctrl+Z) */
        else if (msg.type == MSG_TYPE_SIGNAL) {
            int sig = *(int *)msg.data;
            if (sig == SIGINT) {
                /* If foreground command is executing, forward SIGINT to its process group */
                if (tab->jobs.fg_pgid > 0) {
                    kill(-tab->jobs.fg_pgid, SIGINT);
                } else if (tab->is_busy) {
                    tab->stop_multiwatch = 1;
                } else if (tab->in_ctrl_r) {
                    tab->in_ctrl_r = 0;
                    tab->search_term[0] = '\0';
                    term_write(out_fd, "^C\n", 3);
                    print_prompt(tab, out_fd);
                } else {
                    /* Cancel current input line */
                    tab->input_line[0] = '\0';
                    tab->multiline_accum[0] = '\0';
                    tab->cursor_pos = 0;
                    tab->in_multiline = 0;
                    tab->waiting_completion_choice = 0;
                    term_write(out_fd, "^C\n", 3);
                    print_prompt(tab, out_fd);
                }
            } else if (sig == SIGTSTP) {
                /* If foreground command is executing, forward SIGTSTP to suspend it */
                if (tab->jobs.fg_pgid > 0) {
                    kill(-tab->jobs.fg_pgid, SIGTSTP);
                }
            }
        }
        /* Message 3: Explicit Exit request */
        else if (msg.type == MSG_TYPE_EXIT) {
            tab->tab_exited = 1;
            break;
        }
    }

    /* Close pipes on exit so GUI receives EOF on read() */
    if (tab->tab_out_fd >= 0) {
        close(tab->tab_out_fd);
        tab->tab_out_fd = -1;
    }
    if (tab->tab_in_fd >= 0) {
        close(tab->tab_in_fd);
        tab->tab_in_fd = -1;
    }
}

/*
 * -------------------------------------------------------------------------------------
 * Function: shell_thread_worker
 *
 * Purpose:
 *   Entry function for POSIX thread mode (pthread_create).
 *   Executes shell_worker_loop() in a detached or joinable thread context.
 *
 * Parameters:
 *   @arg: Pointer to TabState cast to void*.
 *
 * Returns:
 *   void*: NULL upon thread completion.
 * -------------------------------------------------------------------------------------
 */
void *shell_thread_worker(void *arg) {
    TabState *tab = (TabState *)arg;
    shell_worker_loop(tab, tab->tab_out_fd, tab->tab_in_fd);
    return NULL;
}

/*
 * -------------------------------------------------------------------------------------
 * Function: shell_process_worker
 *
 * Purpose:
 *   Entry function for process mode (fork).
 *   Executes shell_worker_loop() in a dedicated child process context.
 *
 * Parameters:
 *   @tab: Pointer to TabState.
 *
 * Returns:
 *   void: Calls exit(0) on termination.
 * -------------------------------------------------------------------------------------
 */
void shell_process_worker(TabState *tab) {
    shell_worker_loop(tab, tab->tab_out_fd, tab->tab_in_fd);
    exit(0);
}

/*
 * -------------------------------------------------------------------------------------
 * Function: shell_tab_start
 *
 * Purpose:
 *   Launches a tab's background execution worker:
 *     1. Creates two UNIX pipes for full-duplex communication:
 *        - p_gui_to_tab: GUI sends key events and signals to tab worker.
 *        - p_tab_to_gui: Tab worker sends terminal output text to GUI.
 *     2. In MODE_PROCESS: forks a child process and calls shell_process_worker().
 *     3. In MODE_THREAD: spawns a POSIX thread via pthread_create() running shell_thread_worker().
 *
 * Parameters:
 *   @tab: Pointer to TabState.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
void shell_tab_start(TabState *tab) {
    if (!tab) return;

    int p_gui_to_tab[2];
    int p_tab_to_gui[2];

    /* pipe() creates unidirectional data channels */
    if (pipe(p_gui_to_tab) < 0 || pipe(p_tab_to_gui) < 0) {
        perror("tab pipe failed");
        return;
    }

    tab->to_tab_fd = p_gui_to_tab[1];   /* GUI writes to this end */
    tab->tab_in_fd = p_gui_to_tab[0];   /* Tab reads from this end */

    tab->from_tab_fd = p_tab_to_gui[0]; /* GUI reads from this end */
    tab->tab_out_fd = p_tab_to_gui[1];  /* Tab writes to this end */

    if (tab->mode == MODE_PROCESS) {
        pid_t pid = fork();
        if (pid == 0) {
            /* Child process: closes unused ends of pipes */
            close(tab->to_tab_fd);
            close(tab->from_tab_fd);
            shell_process_worker(tab);
        } else {
            /* Parent GUI process */
            tab->process_pid = pid;
            close(tab->tab_in_fd);
            close(tab->tab_out_fd);
        }
    } else {
        /* Thread mode: pthread_create() spawns concurrent worker thread */
        pthread_create(&tab->thread, NULL, shell_thread_worker, tab);
    }
}

/*
 * -------------------------------------------------------------------------------------
 * Function: shell_tab_send_key
 *
 * Purpose:
 *   Transmits a TermKeyEvent from the GUI over the IPC pipe to the tab worker.
 *
 * Parameters:
 *   @tab: Pointer to TabState.
 *   @ev:  Pointer to TermKeyEvent.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
void shell_tab_send_key(TabState *tab, const TermKeyEvent *ev) {
    if (!tab || tab->to_tab_fd < 0 || !ev) return;
    IpcMessage msg;
    msg.type = MSG_TYPE_KEY;
    msg.len = sizeof(TermKeyEvent);
    memcpy(msg.data, ev, sizeof(TermKeyEvent));
    term_write(tab->to_tab_fd, &msg, sizeof(msg));
}

/*
 * -------------------------------------------------------------------------------------
 * Function: shell_tab_send_signal
 *
 * Purpose:
 *   Transmits a POSIX signal (SIGINT or SIGTSTP) from GUI over the IPC pipe to the tab worker.
 *
 * Parameters:
 *   @tab: Pointer to TabState.
 *   @sig: Signal number (SIGINT or SIGTSTP).
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
void shell_tab_send_signal(TabState *tab, int sig) {
    if (!tab || tab->to_tab_fd < 0) return;
    IpcMessage msg;
    msg.type = MSG_TYPE_SIGNAL;
    msg.len = sizeof(int);
    memcpy(msg.data, &sig, sizeof(int));
    term_write(tab->to_tab_fd, &msg, sizeof(msg));
}

/*
 * -------------------------------------------------------------------------------------
 * Function: shell_tab_close
 *
 * Purpose:
 *   Shuts down a tab worker cleanly and frees resources:
 *     1. Marks tab_exited = 1.
 *     2. Writes MSG_TYPE_EXIT message over pipe to signal worker loop to break.
 *     3. Closes file descriptors.
 *     4. In Process mode: sends SIGTERM and reaps zombie with waitpid(..., WNOHANG).
 *     5. In Thread mode: calls pthread_join() to ensure thread finishes before TabState is freed,
 *        preventing segfaults and race conditions.
 *
 * Parameters:
 *   @tab: Pointer to TabState.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
void shell_tab_close(TabState *tab) {
    if (!tab) return;
    tab->tab_exited = 1;
    if (tab->to_tab_fd >= 0) {
        IpcMessage msg;
        msg.type = MSG_TYPE_EXIT;
        msg.len = 0;
        term_write(tab->to_tab_fd, &msg, sizeof(msg));
        close(tab->to_tab_fd);
        tab->to_tab_fd = -1;
    }
    if (tab->from_tab_fd >= 0) {
        close(tab->from_tab_fd);
        tab->from_tab_fd = -1;
    }

    if (tab->mode == MODE_PROCESS) {
        if (tab->process_pid > 0) {
            kill(tab->process_pid, SIGTERM);
            waitpid(tab->process_pid, NULL, WNOHANG);
        }
    } else {
        if (tab->thread) {
            pthread_join(tab->thread, NULL); /* Wait for thread termination */
            tab->thread = 0;
        }
    }
}
