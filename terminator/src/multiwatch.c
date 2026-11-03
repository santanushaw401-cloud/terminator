/*
 * ==============================================================================
 * Project:      Terminator - A Custom Linux Terminal & Shell
 * File:         src/multiwatch.c
 * Purpose:      Parallel command execution & live monitoring (`multiWatch`).
 * Description:  Executes a list of commands simultaneously across concurrent forked
 *               processes, redirects their stdout/stderr through individual UNIX pipes,
 *               polls ready pipes using select() without busy-waiting, prints timestamped
 *               output blocks when commands finish, and auto-repeats every 2 seconds
 *               until the user interrupts with Ctrl+C.
 *
 * Operating System / Viva Concepts:
 * 1. How does multiWatch run commands concurrently?
 *    - Uses a loop over all parsed command strings calling pipe(pfd) and fork().
 *    - In each child process, dup2() redirects STDOUT_FILENO (1) and STDERR_FILENO (2)
 *      into the pipe, and execl() replaces the child's image with /bin/sh -c "cmd".
 *    - All child processes run in parallel on separate CPU cores!
 * 2. How does select() eliminate CPU busy-waiting?
 *    - Instead of constantly checking read() in an infinite loop (which burns 100% CPU),
 *      select() registers the read ends of all command pipes into an fd_set.
 *    - The operating system kernel suspends the parent process until at least one pipe
 *      has output ready, consuming 0% idle CPU cycles.
 * 3. How does Ctrl+C cancellation work?
 *    - in_fd (the pipe from the GUI) is also added to the select() fd_set.
 *    - If the user presses Ctrl+C, the GUI writes a SIGINT packet to in_fd, which wakes up
 *      select() and cleanly terminates all running children with kill(pid, SIGINT).
 * ==============================================================================
 */

#include "multiwatch.h" /* Declarations for WatchedCommand, MAX_WATCH_CMDS, and prototypes */

/*
 * Function:    multiwatch_parse_args
 * Purpose:     Parses an argument string formatted like ["cmd1", "cmd2", ...] into individual commands.
 * Parameters:  arg_str  - The raw string typed after 'multiWatch' (e.g., '["ls -l", "date"]').
 *              cmds     - 2D array where extracted command strings will be stored.
 *              max_cmds - Maximum number of commands the array can hold (MAX_WATCH_CMDS).
 * Returns:     int      - Count of successfully extracted command strings.
 */
int multiwatch_parse_args(const char *arg_str, char cmds[][MAX_CMD_LEN], int max_cmds) {
    if (!arg_str) return 0;

    int cmd_count = 0;
    /* Find optional opening bracket '[' */
    const char *p = strchr(arg_str, '[');
    if (!p) p = arg_str;
    else p++; /* Advance past '[' */

    while (*p && cmd_count < max_cmds) {
        /* Skip leading whitespace, newlines, and comma separators */
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n') p++;
        /* Stop if we hit the closing bracket ']' or end of string */
        if (*p == ']' || *p == '\0') break;

        /* Case 1: Quoted command string ("cmd" or 'cmd') */
        if (*p == '"' || *p == '\'') {
            char quote = *p++; /* Remember quote type (' or ") */
            int len = 0;
            while (*p && *p != quote && len < MAX_CMD_LEN - 1) {
                /* Handle backslash escape sequences like \" */
                if (*p == '\\' && *(p + 1)) {
                    p++;
                }
                cmds[cmd_count][len++] = *p++;
            }
            if (*p == quote) p++; /* Skip closing quote */
            cmds[cmd_count][len] = '\0';
            if (len > 0) cmd_count++;
        }
        /* Case 2: Unquoted command string */
        else {
            int len = 0;
            while (*p && *p != ',' && *p != ']' && len < MAX_CMD_LEN - 1) {
                cmds[cmd_count][len++] = *p++;
            }
            cmds[cmd_count][len] = '\0';
            /* Trim trailing whitespace */
            while (len > 0 && (cmds[cmd_count][len - 1] == ' ' || cmds[cmd_count][len - 1] == '\t')) {
                cmds[cmd_count][--len] = '\0';
            }
            if (len > 0) cmd_count++;
        }
    }

    return cmd_count; /* Return number of commands parsed */
}

/*
 * Function:    print_watch_output
 * Purpose:     Prints a formatted timestamped header and output box for a finished command.
 * Parameters:  out_fd - File descriptor to write the output to (leading to GUI).
 *              cmd    - Command name string.
 *              ts     - UNIX timestamp (seconds since epoch) when the command finished.
 *              output - Captured stdout/stderr text of the command.
 * Returns:     void.
 *
 * Example Output:
 *   "date", 1729000000:
 *   ----------------------------------------------------
 *   Tue Nov  3 22:30:00 IST 2026
 *   ----------------------------------------------------
 */
static void print_watch_output(int out_fd, const char *cmd, time_t ts, const char *output) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "\"%s\", %ld:\n----------------------------------------------------\n",
                        cmd, (long)ts);
    write(out_fd, header, hlen);

    /* Write captured process output text */
    if (output && strlen(output) > 0) {
        write(out_fd, output, strlen(output));
        /* Ensure output ends with a newline */
        if (output[strlen(output) - 1] != '\n') {
            write(out_fd, "\n", 1);
        }
    }

    const char *footer = "----------------------------------------------------\n";
    write(out_fd, footer, strlen(footer));
}

/*
 * Function:    multiwatch_run
 * Purpose:     Main engine of multiWatch. Concurrently executes and monitors all commands.
 * Parameters:  cmds       - 2D array of command strings.
 *              cmd_count  - Number of commands to execute.
 *              out_fd     - Pipe to write terminal display output.
 *              in_fd      - Pipe to receive keystrokes/signals from GUI (for Ctrl+C).
 *              stop_flag  - Volatile flag set to 1 when Ctrl+C is pressed.
 *              cwd        - Current working directory where child commands should run.
 * Returns:     void.
 */
void multiwatch_run(char cmds[][MAX_CMD_LEN], int cmd_count, int out_fd, int in_fd, volatile int *stop_flag, const char *cwd) {
    if (cmd_count <= 0) return;

    /* Initialize tracking structure for each command */
    WatchedCommand watched[MAX_WATCH_CMDS];
    memset(watched, 0, sizeof(watched));

    for (int i = 0; i < cmd_count; i++) {
        strncpy(watched[i].cmd, cmds[i], MAX_CMD_LEN - 1);
        watched[i].finish_time = 0; /* 0 means never run yet */
        watched[i].pipe_fd = -1;
    }

    /* Loop until user presses Ctrl+C (*stop_flag becomes 1) */
    while (!(*stop_flag)) {
        time_t now = time(NULL);

        /*
         * 1. Spawn Child Processes:
         * Check each command: if it is not currently running and at least 2 seconds
         * have elapsed since it last finished (or it has never run yet), fork and run it!
         */
        for (int i = 0; i < cmd_count; i++) {
            if (*stop_flag) break;
            if (!watched[i].is_running && (watched[i].finish_time == 0 || (now - watched[i].finish_time) >= 2)) {
                int pfd[2];
                /* Create unidirectional pipe: pfd[0] is read end, pfd[1] is write end */
                if (pipe(pfd) < 0) continue;

                pid_t pid = fork(); /* Clone process */
                if (pid == 0) {
                    /* ================= Child Process ================= */
                    if (cwd && strlen(cwd) > 0) {
                        chdir(cwd); /* Switch to tab's working directory */
                    }
                    close(pfd[0]); /* Child does not read from pipe; close read end */

                    /*
                     * dup2(oldfd, newfd):
                     * Duplicates pfd[1] onto standard output (1) and standard error (2).
                     * Any printf or error messages from the command flow into our pipe!
                     */
                    dup2(pfd[1], STDOUT_FILENO);
                    dup2(pfd[1], STDERR_FILENO);
                    close(pfd[1]); /* Close original write end after duplication */

                    /* Execute command via standard POSIX shell */
                    execl("/bin/sh", "sh", "-c", watched[i].cmd, (char *)NULL);
                    exit(127); /* If exec fails, exit child with error status 127 */
                } else if (pid > 0) {
                    /* ================= Parent Shell ================= */
                    close(pfd[1]); /* Parent does not write to pipe; close write end */
                    watched[i].pid = pid;
                    watched[i].pipe_fd = pfd[0]; /* Keep read end to capture output */
                    watched[i].is_running = 1;
                    watched[i].out_len = 0;
                    watched[i].output[0] = '\0';
                } else {
                    /* Fork failed */
                    close(pfd[0]);
                    close(pfd[1]);
                }
            }
        }

        if (*stop_flag) break;

        /*
         * 2. Non-blocking I/O Multiplexing via select():
         * Add all active command pipes AND the GUI input pipe (in_fd) into rfds.
         */
        fd_set rfds;
        FD_ZERO(&rfds);
        int max_fd = -1;

        /* Listen for user keystrokes / Ctrl+C from GUI */
        if (in_fd >= 0) {
            FD_SET(in_fd, &rfds);
            max_fd = in_fd;
        }

        /* Listen to output pipes of all currently running commands */
        for (int i = 0; i < cmd_count; i++) {
            if (watched[i].is_running && watched[i].pipe_fd >= 0) {
                FD_SET(watched[i].pipe_fd, &rfds);
                if (watched[i].pipe_fd > max_fd) {
                    max_fd = watched[i].pipe_fd;
                }
            }
        }

        /* Sleep up to 100 milliseconds or until any pipe produces output */
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        int ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue; /* Interrupted by signal; continue loop */
            break;
        }

        /*
         * 3. Check for User Cancellation (Ctrl+C):
         * If in_fd has data, the GUI forwarded an event or signal.
         */
        if (in_fd >= 0 && FD_ISSET(in_fd, &rfds)) {
            IpcMessage msg;
            ssize_t n = read(in_fd, &msg, sizeof(msg));
            if (n > 0) {
                if (msg.type == MSG_TYPE_SIGNAL || msg.type == MSG_TYPE_KEY) {
                    *stop_flag = 1; /* Set stop flag to exit multiWatch */
                    break;
                }
            }
        }

        /*
         * 4. Read Output from Ready Command Pipes:
         */
        for (int i = 0; i < cmd_count; i++) {
            if (watched[i].is_running && watched[i].pipe_fd >= 0 && FD_ISSET(watched[i].pipe_fd, &rfds)) {
                char tmp[1024];
                ssize_t n = read(watched[i].pipe_fd, tmp, sizeof(tmp) - 1);
                if (n > 0) {
                    tmp[n] = '\0';
                    /* Append newly arrived bytes into command's output buffer */
                    int avail = sizeof(watched[i].output) - watched[i].out_len - 1;
                    if (avail > 0) {
                        int copy_len = (n < avail) ? n : avail;
                        memcpy(watched[i].output + watched[i].out_len, tmp, copy_len);
                        watched[i].out_len += copy_len;
                        watched[i].output[watched[i].out_len] = '\0';
                    }
                } else if (n == 0) {
                    /*
                     * n == 0 indicates End-Of-File (EOF):
                     * The child process has closed its write end of the pipe and finished execution!
                     */
                    close(watched[i].pipe_fd);
                    watched[i].pipe_fd = -1;
                    watched[i].is_running = 0;
                    watched[i].finish_time = time(NULL); /* Record completion timestamp */
                    waitpid(watched[i].pid, NULL, 0);     /* Reap zombie child process */

                    /* Print formatted output block with command name and timestamp */
                    print_watch_output(out_fd, watched[i].cmd, watched[i].finish_time, watched[i].output);
                }
            }
        }
    }

    /*
     * 5. Clean Teardown on Ctrl+C:
     * Kill any commands that are still actively running.
     */
    for (int i = 0; i < cmd_count; i++) {
        if (watched[i].is_running) {
            if (watched[i].pid > 0) {
                kill(watched[i].pid, SIGINT);  /* Request graceful termination */
                kill(watched[i].pid, SIGKILL); /* Force kill if still active */
                waitpid(watched[i].pid, NULL, WNOHANG); /* Reap child */
            }
            if (watched[i].pipe_fd >= 0) {
                close(watched[i].pipe_fd);
            }
        }
    }

    /* Print confirmation to terminal */
    write(out_fd, "^C\n", 3);
}
