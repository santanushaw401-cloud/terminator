/*
 * =====================================================================================
 * File: include/shell.h
 * Project: Terminator - Custom Tabbed Terminal Emulator & Shell
 *
 * Educational Purpose & Viva Guide:
 * This header declares the core state structure for each terminal tab (TabState)
 * and the function prototypes for tab worker lifecycle, inter-process communication,
 * and command line execution.
 * =====================================================================================
 */

#ifndef SHELL_H
#define SHELL_H

#include "terminator.h"   /* Core constants, IPC structures, and standard headers */
#include "buffer.h"       /* TermBuffer screen buffer structure */
#include "jobcontrol.h"   /* Background job control and JobList structure */
#include "completion.h"   /* Tab completion primitives and CompletionResult structure */

/*
 * Structure: TabState
 * Purpose:
 *   Contains the complete, isolated runtime context for a single terminal tab:
 *   - Tab identity & focus: id (recycled integer 1..N), is_active.
 *   - Filesystem state: cwd (current working directory), old_pwd (for 'cd -').
 *   - Line-editing state: input_line buffer, cursor_pos column index.
 *   - Multiline accumulator: multiline_accum buffer for incomplete quotes/continuations.
 *   - Auto-completion interactive state: waiting_completion_choice, completion_res.
 *   - History search & browsing state: in_ctrl_r, search_term, hist_browse_idx, saved_input.
 *   - Background jobs list: jobs.
 *   - IPC Pipes: to_tab_fd, from_tab_fd, tab_in_fd, tab_out_fd.
 *   - Worker handle: process_pid (for process mode) or thread (for thread mode).
 *   - Lifecycle and status flags: tab_exited, is_busy, stop_multiwatch, last_status.
 */
typedef struct TabState {
    int id;                                /* Tab ID number displayed in the tab header */
    int is_active;                         /* 1 if this tab currently has keyboard focus */
    char cwd[MAX_PATH_LEN];                /* Current working directory of this tab */
    char old_pwd[MAX_PATH_LEN];            /* Previous directory for 'cd -' command */
    char input_line[MAX_CMD_LEN];          /* Active line text being typed by user */
    int cursor_pos;                        /* Character index of the cursor in input_line */
    int last_status;                       /* Exit status of the most recently executed command ($?) */

    /* Multiline command accumulation buffer */
    char multiline_accum[MAX_CMD_LEN * 4]; /* Accumulates multiple lines when quotes are unclosed */
    int in_multiline;                      /* 1 if currently awaiting multiline continuation */

    /* Tab auto-completion selection state */
    int waiting_completion_choice;         /* 1 if waiting for user to pick 1..9 from match list */
    CompletionResult completion_res;       /* Holds matched filenames and common prefix */

    /* Ctrl+R reverse interactive history search state */
    int in_ctrl_r;                         /* 1 if in Ctrl+R reverse history search mode */
    char search_term[MAX_CMD_LEN];         /* Query string typed during Ctrl+R */

    /* History browsing state (Up / Down arrows) */
    int hist_browse_idx;                   /* Index into history table (-1 if not browsing) */
    char saved_input[MAX_CMD_LEN];         /* Preserves user's typed line when browsing history */

    /* Job control state */
    JobList jobs;                          /* Linked/array list of active background jobs */

    /* Full-duplex UNIX pipes between GUI and Tab Shell worker */
    int to_tab_fd;                         /* GUI writes keystrokes and signals to tab */
    int from_tab_fd;                       /* GUI reads terminal output text from tab */
    int tab_in_fd;                         /* Tab reads keystrokes from GUI */
    int tab_out_fd;                        /* Tab writes terminal output to GUI */

    /* Worker process / thread identifiers */
    pid_t process_pid;                     /* Process ID if running in MODE_PROCESS */
    pthread_t thread;                      /* POSIX thread handle if running in MODE_THREAD */

    /* Volatile synchronization flags */
    volatile int stop_multiwatch;          /* Flag to abort active multiwatch monitor */
    volatile int tab_exited;               /* Flag indicating shell worker has terminated */
    volatile int is_busy;                  /* Flag indicating a command is actively running */

    ExecutionMode mode;                    /* MODE_THREAD or MODE_PROCESS */
} TabState;

/*
 * Tab Lifecycle & IPC Prototypes:
 */

/* Initializes a tab state structure with default paths, jobs list, and ID */
void shell_tab_init(TabState *tab, int id, ExecutionMode mode);

/* Creates IPC pipes and spawns background worker thread or child process */
void shell_tab_start(TabState *tab);

/* Sends a keystroke event (TermKeyEvent) from GUI to the tab worker */
void shell_tab_send_key(TabState *tab, const TermKeyEvent *ev);

/* Sends a POSIX signal (SIGINT or SIGTSTP) from GUI to the tab worker */
void shell_tab_send_signal(TabState *tab, int sig);

/* Closes IPC pipes, joins thread / waits for process, and cleans up tab */
void shell_tab_close(TabState *tab);

/* Worker entry function for thread mode (pthread_create) */
void *shell_thread_worker(void *arg);

/* Worker entry function for process mode (fork) */
void shell_process_worker(TabState *tab);

/*
 * Core Command Execution Prototype:
 * Expands variables, parses pipelines, handles builtins/scripts, forks and execs binaries.
 */
int shell_execute_line(TabState *tab, const char *raw_line, int out_fd, int in_fd);

#endif /* SHELL_H */
