/*
 * ==============================================================================
 * Project:      Terminator - A Custom Linux Terminal & Shell
 * File:         src/builtins.c
 * Purpose:      Implementation of internal shell commands (Built-ins).
 * Description:  Executes commands that cannot or should not be executed as external
 *               child binaries, such as `cd`, `exit`, `history`, `jobs`, `fg`, `bg`,
 *               `multiWatch`, and `source`/`bash`.
 *
 * Core Concept / Viva Questions:
 * 1. Why must `cd` be a built-in command?
 *    - In UNIX/Linux, every process has its own Current Working Directory (CWD).
 *    - When an external program (e.g. `/bin/ls`) runs, the shell calls fork() to create
 *      a child process. If `cd` were an external program, it would change the directory
 *      of the child process, which then terminates, leaving the shell's CWD unchanged.
 *    - Therefore, `cd` must execute inside the shell's own process memory to update `tab->cwd`.
 * 2. Why must `exit` be a built-in?
 *    - An external program cannot tell the parent shell process or thread to terminate.
 * 3. What is the OSC sequence in update_tab_cwd_notification?
 *    - OSC stands for "Operating System Command" (\033]0;title\007).
 *    - The shell sends this text packet through the pipe to inform the X11 GUI that the
 *      tab's working directory has changed, so the GUI can redraw the tab title bar!
 * ==============================================================================
 */

#include "builtins.h"    /* Declarations for built-in command checkers and dispatchers */
#include "shell.h"       /* TabState structure, pipe file descriptors, and prompt state */
#include "history.h"     /* Functions to query and print the shared command history */
#include "multiwatch.h"  /* MultiWatch parallel command monitoring subsystem */
#include "script.h"      /* Bash script interpreter for 'source' and 'bash' scripts */

/*
 * Function:    builtin_is_builtin
 * Purpose:     Determines whether a command name matches any recognized shell built-in.
 * Parameters:  cmd - The command string entered by the user (e.g. "cd", "ls", "jobs").
 * Returns:     int - Returns 1 (true) if the command is a built-in, 0 (false) if external.
 * Significance:Used by the parser to decide whether to execute directly or fork an external binary.
 */
int builtin_is_builtin(const char *cmd) {
    if (!cmd) return 0; /* Safety check: return false for null pointers */

    /* Check each supported built-in keyword */
    if (strcmp(cmd, "cd") == 0) return 1;         /* Change directory */
    if (strcmp(cmd, "exit") == 0) return 1;       /* Exit current tab / shell */
    if (strcmp(cmd, "history") == 0) return 1;    /* Show recent command history */
    if (strcmp(cmd, "multiWatch") == 0) return 1; /* Concurrent command monitor */
    if (strcmp(cmd, "jobs") == 0) return 1;       /* List background / stopped jobs */
    if (strcmp(cmd, "fg") == 0) return 1;         /* Bring job to foreground */
    if (strcmp(cmd, "bg") == 0) return 1;         /* Resume job in background */
    if (strcmp(cmd, "source") == 0 || strcmp(cmd, ".") == 0) return 1; /* Execute script in current shell */
    if (strcmp(cmd, "bash") == 0) return 1;       /* Run custom script interpreter */

    return 0; /* Not a built-in command; will be executed via fork/execvp */
}

/*
 * Function:    update_tab_cwd_notification
 * Purpose:     Notifies the parent GUI process of a directory change via an ANSI OSC escape string.
 * Parameters:  tab    - Pointer to the active TabState.
 *              out_fd - Write-end file descriptor of the pipe leading to the GUI.
 * Returns:     void.
 * Explanation: Writes "\033]0;<path>\007" to out_fd. The GUI loop intercepts this and updates the tab bar.
 */
static void update_tab_cwd_notification(TabState *tab, int out_fd) {
    char osc[MAX_PATH_LEN + 16];
    /* Format standard XTerm title sequence: ESC ] 0 ; <title> BEL */
    snprintf(osc, sizeof(osc), "\033]0;%s\007", tab->cwd);
    /* Write escape sequence to the pipe connected to the GUI */
    term_write(out_fd, osc, strlen(osc));
}

/*
 * Function:    builtin_execute
 * Purpose:     Dispatches and executes the appropriate built-in routine for the command.
 * Parameters:  tab       - Pointer to current tab state (holds CWD, old_pwd, job list).
 *              args      - Array of argument strings (args[0] is command, args[1] first param).
 *              arg_count - Number of elements in args array.
 *              out_fd    - Output file descriptor where text output should be written.
 *              in_fd     - Input file descriptor for keystrokes or data.
 * Returns:     int       - Execution exit code (0 for success, non-zero for error).
 */
int builtin_execute(TabState *tab, char **args, int arg_count, int out_fd, int in_fd) {
    if (!args || arg_count <= 0) return 0; /* Return 0 if no command given */

    const char *cmd = args[0]; /* Command name to check */

    /* -------------------------------------------------------------------------
     * 1. Built-in: cd [directory]
     * Changes current working directory of the tab.
     * Supports:
     *   - cd           -> changes to $HOME
     *   - cd ~         -> changes to $HOME
     *   - cd ~/path    -> expands relative to $HOME
     *   - cd -         -> toggles back to previous working directory (OLDPWD)
     *   - cd /abs/path -> changes to absolute path
     *   - cd rel/path  -> changes to relative path under tab->cwd
     * ------------------------------------------------------------------------- */
    if (strcmp(cmd, "cd") == 0) {
        char target_dir[MAX_PATH_LEN * 2] = {0};
        const char *home = getenv("HOME"); /* Retrieve user's home directory from environment */
        if (!home) home = "/";             /* Fallback to root if HOME is undefined */

        /* Case: 'cd' with no arguments or 'cd ~' -> navigate to HOME */
        if (arg_count == 1 || strcmp(args[1], "~") == 0) {
            snprintf(target_dir, sizeof(target_dir), "%s", home);
        }
        /* Case: 'cd -' -> navigate to previous directory (stored in old_pwd) */
        else if (strcmp(args[1], "-") == 0) {
            if (strlen(tab->old_pwd) == 0) {
                const char *err = "terminator: cd: OLDPWD not set\n";
                term_write(out_fd, err, strlen(err));
                return 1; /* Error code 1 */
            }
            snprintf(target_dir, sizeof(target_dir), "%s", tab->old_pwd);
            /* Standard bash prints the directory name when toggling with cd - */
            char msg[MAX_PATH_LEN + 2];
            snprintf(msg, sizeof(msg), "%s\n", target_dir);
            term_write(out_fd, msg, strlen(msg));
        }
        /* Case: 'cd ~/something' -> replace '~/' with home directory */
        else if (strncmp(args[1], "~/", 2) == 0) {
            snprintf(target_dir, sizeof(target_dir), "%s/%s", home, args[1] + 2);
        }
        /* Case: 'cd /absolute/path' */
        else if (args[1][0] == '/') {
            snprintf(target_dir, sizeof(target_dir), "%s", args[1]);
        }
        /* Case: 'cd relative/path' -> concatenate with current tab->cwd */
        else {
            snprintf(target_dir, sizeof(target_dir), "%s/%s", tab->cwd, args[1]);
        }

        /*
         * Canonicalize path using POSIX realpath(target, NULL):
         * Resolves all symbolic links, '.', '..', and extra slashes.
         * Passing NULL dynamically allocates memory of exact size, preventing buffer overflows!
         */
        char *resolved = realpath(target_dir, NULL);
        if (!resolved) {
            /* Path does not exist or access was denied */
            char err[512];
            snprintf(err, sizeof(err), "terminator: cd: %s: No such file or directory\n", args[1]);
            term_write(out_fd, err, strlen(err));
            return 1;
        }

        /* Verify using stat() system call that target is indeed a directory, not a regular file */
        struct stat st;
        if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
            char err[512];
            snprintf(err, sizeof(err), "terminator: cd: %s: Not a directory\n", args[1]);
            term_write(out_fd, err, strlen(err));
            free(resolved); /* Free dynamically allocated buffer from realpath */
            return 1;
        }

        /* Save current directory into old_pwd before updating */
        snprintf(tab->old_pwd, sizeof(tab->old_pwd), "%s", tab->cwd);
        /* Update tab's current working directory to the newly resolved canonical path */
        snprintf(tab->cwd, sizeof(tab->cwd), "%s", resolved);
        /* Update POSIX environment variable PWD for external utilities */
        setenv("PWD", tab->cwd, 1);

        /* In process mode, also call chdir() on the child process itself */
        if (tab->mode == MODE_PROCESS) {
            chdir(tab->cwd);
        }

        /* Notify GUI to redraw tab title bar with new directory */
        update_tab_cwd_notification(tab, out_fd);
        free(resolved); /* Clean up allocated path */
        return 0;       /* Return 0 on success */
    }

    /* -------------------------------------------------------------------------
     * 2. Built-in: exit [status]
     * Signals the current tab shell loop to terminate.
     * ------------------------------------------------------------------------- */
    if (strcmp(cmd, "exit") == 0) {
        int code = 0;
        if (arg_count > 1) {
            code = atoi(args[1]); /* Parse optional numeric exit code */
        }
        tab->last_status = code;
        tab->tab_exited = 1;     /* Set flag so the worker loop exits cleanly */
        return code;
    }

    /* -------------------------------------------------------------------------
     * 3. Built-in: history [n]
     * Displays the most recent n commands from shared history (default 10).
     * ------------------------------------------------------------------------- */
    if (strcmp(cmd, "history") == 0) {
        int count = HISTORY_DISPLAY_COUNT; /* Default: 10 */
        if (arg_count > 1) {
            count = atoi(args[1]);         /* User-specified number of entries */
        }
        /* Allocate temporary buffer to format history list */
        char *hbuf = (char *)malloc(131072);
        if (hbuf) {
            history_print_recent(count, hbuf, 131072);
            term_write(out_fd, hbuf, strlen(hbuf));
            free(hbuf);
        }
        return 0;
    }

    /* -------------------------------------------------------------------------
     * 4. Built-in: jobs
     * Lists active and stopped background jobs for this tab.
     * ------------------------------------------------------------------------- */
    if (strcmp(cmd, "jobs") == 0) {
        char jbuf[4096];
        /* Check status of any child jobs that might have completed in background */
        job_check_status(&tab->jobs, NULL, 0);
        /* Format and print jobs table */
        job_list_print(&tab->jobs, jbuf, sizeof(jbuf));
        term_write(out_fd, jbuf, strlen(jbuf));
        return 0;
    }

    /* -------------------------------------------------------------------------
     * 5. Built-in: fg [%job_id]
     * Brings a background or stopped job to the foreground.
     * ------------------------------------------------------------------------- */
    if (strcmp(cmd, "fg") == 0) {
        int jid = -1;
        if (arg_count > 1) {
            /* Support both %1 and 1 syntax */
            if (args[1][0] == '%') jid = atoi(args[1] + 1);
            else jid = atoi(args[1]);
        }
        char msg[512];
        pid_t fg_pgid = 0;
        char fg_cmd[MAX_CMD_LEN] = {0};

        /* Call job_fg to send SIGCONT and configure process group */
        if (job_fg(&tab->jobs, jid, msg, sizeof(msg), &fg_pgid, fg_cmd) == 0) {
            term_write(out_fd, msg, strlen(msg));
            tab->jobs.fg_pgid = fg_pgid;
            strncpy(tab->jobs.fg_cmd, fg_cmd, sizeof(tab->jobs.fg_cmd) - 1);

            /* Wait for process group to finish or be stopped with Ctrl+Z */
            int status;
            pid_t p = waitpid(-fg_pgid, &status, WUNTRACED);
            if (p > 0 && WIFSTOPPED(status)) {
                /* Job was suspended with Ctrl+Z: add back to jobs list */
                int nid = job_add(&tab->jobs, fg_pgid, fg_cmd, JOB_STOPPED);
                char stop_msg[512];
                snprintf(stop_msg, sizeof(stop_msg), "\n[%d]+ Stopped %s\n", nid, fg_cmd);
                term_write(out_fd, stop_msg, strlen(stop_msg));
            }
            tab->jobs.fg_pgid = 0;
        } else {
            term_write(out_fd, msg, strlen(msg));
            return 1;
        }
        return 0;
    }

    /* -------------------------------------------------------------------------
     * 6. Built-in: bg [%job_id]
     * Resumes a stopped job in the background (sends SIGCONT).
     * ------------------------------------------------------------------------- */
    if (strcmp(cmd, "bg") == 0) {
        int jid = -1;
        if (arg_count > 1) {
            if (args[1][0] == '%') jid = atoi(args[1] + 1);
            else jid = atoi(args[1]);
        }
        char msg[512];
        if (job_bg(&tab->jobs, jid, msg, sizeof(msg)) == 0) {
            term_write(out_fd, msg, strlen(msg));
        } else {
            term_write(out_fd, msg, strlen(msg));
            return 1;
        }
        return 0;
    }

    /* -------------------------------------------------------------------------
     * 7. Built-in: multiWatch ["cmd1", "cmd2", ...]
     * Concurrently executes multiple commands across separate processes,
     * polling output with select() and refreshing every 2 seconds.
     * ------------------------------------------------------------------------- */
    if (strcmp(cmd, "multiWatch") == 0) {
        char watch_cmds[MAX_WATCH_CMDS][MAX_CMD_LEN];
        char all_args[MAX_CMD_LEN] = {0};
        /* Reconstruct argument string into array format */
        for (int i = 1; i < arg_count; i++) {
            strcat(all_args, args[i]);
            if (i + 1 < arg_count) strcat(all_args, " ");
        }

        /* Parse array of quoted command strings */
        int count = multiwatch_parse_args(all_args, watch_cmds, MAX_WATCH_CMDS);
        if (count == 0) {
            const char *err = "terminator: multiWatch: usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n";
            term_write(out_fd, err, strlen(err));
            return 1;
        }

        tab->stop_multiwatch = 0; /* Reset stop flag */
        tab->is_busy = 1;         /* Mark tab as busy so Ctrl+C cancels multiwatch */
        /* Run parallel monitor loop */
        multiwatch_run(watch_cmds, count, out_fd, in_fd, &tab->stop_multiwatch, tab->cwd);
        tab->is_busy = 0;         /* Clear busy flag */
        return 0;
    }

    /* -------------------------------------------------------------------------
     * 8. Built-in: source <file> or . <file>
     * Reads and executes a shell script within the current tab's environment.
     * ------------------------------------------------------------------------- */
    if (strcmp(cmd, "source") == 0 || strcmp(cmd, ".") == 0) {
        if (arg_count < 2) {
            const char *err = "terminator: source: filename argument required\n";
            term_write(out_fd, err, strlen(err));
            return 1;
        }

        char script_path[MAX_PATH_LEN];
        if (args[1][0] == '/') {
            strncpy(script_path, args[1], sizeof(script_path) - 1);
        } else {
            snprintf(script_path, sizeof(script_path), "%s/%s", tab->cwd, args[1]);
        }

        int status = 0;
        int res = script_execute_file(script_path, arg_count - 1, &args[1], out_fd, in_fd, tab->cwd, &status);
        tab->last_status = status;
        return res;
    }

    /* -------------------------------------------------------------------------
     * 9. Built-in: bash <file>
     * Runs our custom bash script interpreter on the specified file.
     * ------------------------------------------------------------------------- */
    if (strcmp(cmd, "bash") == 0) {
        if (arg_count < 2) {
            const char *err = "terminator: bash: script argument required\n";
            term_write(out_fd, err, strlen(err));
            return 1;
        }

        char script_path[MAX_PATH_LEN];
        if (args[1][0] == '/') {
            strncpy(script_path, args[1], sizeof(script_path) - 1);
        } else {
            snprintf(script_path, sizeof(script_path), "%s/%s", tab->cwd, args[1]);
        }

        int status = 0;
        int res = script_execute_file(script_path, arg_count - 1, &args[1], out_fd, in_fd, tab->cwd, &status);
        tab->last_status = status;
        return res;
    }

    return 0;
}
