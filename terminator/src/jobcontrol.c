/*
 * ==============================================================================
 * Project:      Terminator - A Custom Linux Terminal & Shell
 * File:         src/jobcontrol.c
 * Purpose:      Job control subsystem (`&`, `jobs`, `fg`, `bg`, `Ctrl+Z`).
 * Description:  Tracks background processes and suspended foreground pipelines.
 *               Manages process groups (PGID), sends continuation signals (SIGCONT),
 *               checks background process statuses without blocking (waitpid with WNOHANG),
 *               and formats the standard bash `jobs` table (with `+` and `-` indicators).
 *
 * Operating System / Viva Concepts:
 * 1. What is a Process Group ID (PGID)?
 *    - In UNIX, a single command or pipeline (e.g., `cat file | grep text`) can consist
 *      of multiple separate processes.
 *    - By putting all processes of a pipeline into the same Process Group (setpgid),
 *      the shell can pause, resume, or kill the entire pipeline simultaneously using
 *      `kill(-pgid, SIGNAL)` — notice the negative sign!
 * 2. What do the flags in waitpid(-pgid, &status, WNOHANG | WUNTRACED | WCONTINUED) mean?
 *    - `WNOHANG`: Do not block (return immediately if child is still running).
 *    - `WUNTRACED`: Report if child process was stopped (e.g. by Ctrl+Z / SIGTSTP).
 *    - `WCONTINUED`: Report if a stopped child was resumed (via SIGCONT).
 * 3. What do the '+' and '-' symbols mean in `jobs` output?
 *    - `+`: The "current" default job that `fg` or `bg` will operate on if no %id is specified.
 *    - `-`: The "previous" job that was current before the current one.
 * ==============================================================================
 */

#include "jobcontrol.h" /* Declarations for JobList, Job, JobState, and prototypes */

/*
 * Function:    job_list_init
 * Purpose:     Initializes an empty job table for a tab shell.
 * Parameters:  jl - Pointer to the JobList structure to initialize.
 * Returns:     void.
 */
void job_list_init(JobList *jl) {
    if (!jl) return; /* Safety check against NULL pointer */
    memset(jl, 0, sizeof(JobList)); /* Zero out entire table */
    jl->next_id = 1;                /* Job numbers start sequentially from 1 */
    jl->fg_pgid = 0;                /* No foreground job initially active */
    jl->fg_cmd[0] = '\0';
}

/*
 * Function:    job_add
 * Purpose:     Adds a new job (either background '&' or stopped via Ctrl+Z) to the table.
 * Parameters:  jl    - Pointer to JobList.
 *              pgid  - Process Group ID of the child process / pipeline.
 *              cmd   - Command line text string (e.g., "sleep 100").
 *              state - Initial state (JOB_RUNNING or JOB_STOPPED).
 * Returns:     int   - The unique positive integer Job ID assigned, or -1 if table is full.
 */
int job_add(JobList *jl, pid_t pgid, const char *cmd, JobState state) {
    if (!jl || jl->job_count >= MAX_JOBS) return -1; /* Table capacity reached */

    int id = jl->next_id++; /* Allocate next sequential job ID */
    Job *j = &jl->jobs[jl->job_count++];
    j->id = id;
    j->pgid = pgid;
    j->state = state;
    strncpy(j->command, cmd ? cmd : "", MAX_CMD_LEN - 1);
    j->command[MAX_CMD_LEN - 1] = '\0'; /* Ensure null-terminated string */

    return id; /* Return assigned job ID (e.g., 1 for [1]) */
}

/*
 * Function:    job_find_by_id
 * Purpose:     Looks up a job in the list by its sequential job ID.
 * Parameters:  jl - Pointer to JobList.
 *              id - Job ID number to find.
 * Returns:     Job* - Pointer to Job entry, or NULL if not found.
 */
Job *job_find_by_id(JobList *jl, int id) {
    if (!jl) return NULL;
    for (int i = 0; i < jl->job_count; i++) {
        if (jl->jobs[i].id == id) {
            return &jl->jobs[i];
        }
    }
    return NULL;
}

/*
 * Function:    job_find_by_pgid
 * Purpose:     Looks up a job in the list by its process group ID.
 * Parameters:  jl   - Pointer to JobList.
 *              pgid - PGID to search for.
 * Returns:     Job* - Pointer to Job entry, or NULL if not found.
 */
Job *job_find_by_pgid(JobList *jl, pid_t pgid) {
    if (!jl) return NULL;
    for (int i = 0; i < jl->job_count; i++) {
        if (jl->jobs[i].pgid == pgid) {
            return &jl->jobs[i];
        }
    }
    return NULL;
}

/*
 * Function:    job_get_current
 * Purpose:     Gets the "current" default job (the most recently added job).
 * Parameters:  jl - Pointer to JobList.
 * Returns:     Job* - Pointer to the most recent job, or NULL if table is empty.
 */
Job *job_get_current(JobList *jl) {
    if (!jl || jl->job_count == 0) return NULL;
    return &jl->jobs[jl->job_count - 1]; /* Last entry in array */
}

/*
 * Function:    job_remove
 * Purpose:     Removes a completed or foregrounded job from the table.
 * Parameters:  jl - Pointer to JobList.
 *              id - Job ID to remove.
 * Returns:     void.
 */
void job_remove(JobList *jl, int id) {
    if (!jl) return;
    for (int i = 0; i < jl->job_count; i++) {
        if (jl->jobs[i].id == id) {
            /* Shift all subsequent entries left to fill the gap */
            for (int k = i; k < jl->job_count - 1; k++) {
                jl->jobs[k] = jl->jobs[k + 1];
            }
            jl->job_count--;
            break;
        }
    }
}

/*
 * Function:    job_check_status
 * Purpose:     Non-blocking poll of all active jobs using waitpid().
 * Parameters:  jl         - Pointer to JobList.
 *              notify_buf - Optional string buffer to write completion notifications (e.g. "[1]+ Done").
 *              max_len    - Maximum capacity of notify_buf.
 * Returns:     void.
 *
 * Significance:
 * - Called before displaying each shell prompt to clean up zombie background processes
 *   and notify the user if background jobs finished while they were typing.
 */
void job_check_status(JobList *jl, char *notify_buf, int max_len) {
    if (!jl) return;
    if (notify_buf && max_len > 0) notify_buf[0] = '\0';
    int offset = 0;

    int i = 0;
    while (i < jl->job_count) {
        Job *j = &jl->jobs[i];
        int status;
        /*
         * waitpid(-j->pgid, ...):
         * - Negative PGID waits for ANY child process belonging to this process group.
         * - WNOHANG: returns immediately (returns 0 if children are still running).
         * - WUNTRACED: detects if process was stopped by signal.
         * - WCONTINUED: detects if process was resumed.
         */
        pid_t pid = waitpid(-j->pgid, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid > 0) {
            /* Process finished or was killed by signal */
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                if (notify_buf && offset < max_len - 64) {
                    offset += snprintf(notify_buf + offset, max_len - offset,
                                       "[%d]+ Done %s\n", j->id, j->command);
                }
                job_remove(jl, j->id); /* Remove finished job from table */
                continue;              /* Re-check same index because elements shifted */
            } else if (WIFSTOPPED(status)) {
                j->state = JOB_STOPPED; /* Process was paused (e.g., Ctrl+Z) */
            } else if (WIFCONTINUED(status)) {
                j->state = JOB_RUNNING; /* Process was resumed (SIGCONT) */
            }
        } else if (pid < 0 && errno == ECHILD) {
            /* Child process has already exited and been reaped */
            job_remove(jl, j->id);
            continue;
        }
        i++;
    }
}

/*
 * Function:    job_list_print
 * Purpose:     Formats the jobs table into standard UNIX `jobs` command output.
 * Parameters:  jl      - Pointer to JobList.
 *              out_buf - Output buffer to store formatted text table.
 *              max_len - Maximum size of out_buf.
 * Returns:     void.
 *
 * Example Output:
 *   [1]+ Running    sleep 100 &
 *   [2]- Stopped    cat
 */
void job_list_print(const JobList *jl, char *out_buf, int max_len) {
    if (!jl || !out_buf || max_len <= 0) return;
    out_buf[0] = '\0';
    int offset = 0;

    for (int i = 0; i < jl->job_count; i++) {
        const Job *j = &jl->jobs[i];
        /* Current job gets '+', previous job gets '-', older jobs get space ' ' */
        char cur = (i == jl->job_count - 1) ? '+' : (i == jl->job_count - 2 ? '-' : ' ');
        const char *st_str = (j->state == JOB_RUNNING) ? "Running" : "Stopped";
        const char *amp = (j->state == JOB_RUNNING) ? " &" : "";

        int written = snprintf(out_buf + offset, max_len - offset,
                               "[%d]%c %-10s %s%s\n",
                               j->id, cur, st_str, j->command, amp);
        if (written > 0) {
            offset += written;
            if (offset >= max_len - 1) break;
        }
    }
}

/*
 * Function:    job_fg
 * Purpose:     Brings a background or stopped job into the foreground.
 * Parameters:  jl          - Pointer to JobList.
 *              id          - Job ID to foreground (-1 for default current job).
 *              out_buf     - Output buffer for status message.
 *              max_len     - Output buffer capacity.
 *              out_fg_pgid - Pointer to store the PGID of the foregrounded process group.
 *              out_fg_cmd  - Buffer to store the command name string.
 * Returns:     int         - 0 on success, -1 on failure (e.g. no such job).
 */
int job_fg(JobList *jl, int id, char *out_buf, int max_len, pid_t *out_fg_pgid, char *out_fg_cmd) {
    if (!jl) return -1;
    Job *j = NULL;
    if (id > 0) {
        j = job_find_by_id(jl, id);
    } else {
        j = job_get_current(jl); /* Default to latest job */
    }

    if (!j) {
        if (out_buf) snprintf(out_buf, max_len, "terminator: fg: no such job\n");
        return -1;
    }

    /* Print command name to terminal (standard bash behavior for 'fg') */
    if (out_buf) {
        snprintf(out_buf, max_len, "%s\n", j->command);
    }

    if (out_fg_pgid) *out_fg_pgid = j->pgid;
    if (out_fg_cmd) {
        strncpy(out_fg_cmd, j->command, MAX_CMD_LEN - 1);
        out_fg_cmd[MAX_CMD_LEN - 1] = '\0';
    }

    /*
     * Send SIGCONT to the entire process group:
     * -j->pgid instructs kernel to broadcast SIGCONT to all processes in that group,
     * waking them up if they were suspended by SIGTSTP (Ctrl+Z).
     */
    kill(-j->pgid, SIGCONT);
    j->state = JOB_RUNNING;

    int job_id = j->id;
    job_remove(jl, job_id); /* Remove from background table since it is now in foreground */
    return 0;
}

/*
 * Function:    job_bg
 * Purpose:     Resumes a stopped job in the background.
 * Parameters:  jl      - Pointer to JobList.
 *              id      - Job ID to resume (-1 for default current job).
 *              out_buf - Output buffer for confirmation message.
 *              max_len - Output buffer capacity.
 * Returns:     int     - 0 on success, -1 on failure.
 */
int job_bg(JobList *jl, int id, char *out_buf, int max_len) {
    if (!jl) return -1;
    Job *j = NULL;
    if (id > 0) {
        j = job_find_by_id(jl, id);
    } else {
        j = job_get_current(jl);
    }

    if (!j) {
        if (out_buf) snprintf(out_buf, max_len, "terminator: bg: no such job\n");
        return -1;
    }

    /* Send SIGCONT signal to all processes in the group so they resume execution */
    kill(-j->pgid, SIGCONT);
    j->state = JOB_RUNNING;

    /* Print confirmation message: e.g. "[1]+ sleep 100 &" */
    if (out_buf) {
        snprintf(out_buf, max_len, "[%d]+ %s &\n", j->id, j->command);
    }
    return 0;
}
