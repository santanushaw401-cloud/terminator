/*
 * =====================================================================================
 * File: include/terminator.h
 * Project: Terminator - Custom Tabbed Terminal Emulator & Shell
 *
 * Educational Purpose & Viva Guide:
 * This is the central global header file for the Terminator project.
 * It provides:
 *   1. Feature test macros (_GNU_SOURCE) allowing access to POSIX/GNU extensions.
 *   2. Standard library inclusions needed throughout the codebase (I/O, process control,
 *      concurrency, IPC, and POSIX shared memory).
 *   3. Architecture constants (buffer sizes, maximum tabs, history limits, pipeline limits).
 *   4. Inter-Process Communication (IPC) packet definitions (IpcMessage) and message types.
 *   5. Keyboard event definitions (TermKeyCode, TermKeyEvent).
 *   6. Robust I/O helper (term_write) ensuring complete data transmission even across
 *      partial writes and signal interruptions.
 * =====================================================================================
 */

#ifndef TERMINATOR_H
#define TERMINATOR_H

/* Enable POSIX and GNU extensions (e.g., strdup, realpath dynamic allocation, asprintf) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* System and standard C library headers */
#include <stdio.h>      /* Standard I/O (printf, snprintf, fopen, fgets, fclose) */
#include <stdlib.h>     /* Dynamic memory (malloc, calloc, free), environment (getenv, setenv), exit */
#include <string.h>     /* String manipulation (strcmp, strncmp, strlen, strcpy, strchr, memmove) */
#include <unistd.h>     /* POSIX API: fork(), execvp(), pipe(), dup2(), close(), read(), write(), chdir() */
#include <errno.h>      /* Error number definitions and errno variable */
#include <fcntl.h>      /* File control options: open(), O_RDONLY, O_WRONLY, O_CREAT, O_APPEND */
#include <signal.h>     /* Signal handling: signal(), kill(), SIGINT, SIGTSTP, SIGCHLD, SIGPIPE */
#include <sys/types.h>  /* POSIX primitive system data types (pid_t, size_t, ssize_t) */
#include <sys/stat.h>   /* File status and modes: stat(), S_ISREG(), S_ISDIR() */
#include <sys/wait.h>   /* Process synchronization: waitpid(), WIFEXITED, WEXITSTATUS, WUNTRACED */
#include <sys/select.h> /* I/O multiplexing: select(), FD_SET, FD_ZERO, FD_ISSET */
#include <sys/mman.h>   /* POSIX shared memory: mmap(), munmap(), PROT_READ, PROT_WRITE, MAP_SHARED */
#include <semaphore.h>  /* POSIX semaphores: sem_t, sem_init(), sem_wait(), sem_post(), sem_destroy() */
#include <pthread.h>    /* POSIX threads: pthread_t, pthread_create(), pthread_join(), pthread_mutex_t */
#include <dirent.h>     /* Directory navigation: opendir(), readdir(), closedir(), struct dirent */
#include <time.h>       /* Calendar time and timestamps */
#include <ctype.h>      /* Character classification: isdigit(), isalpha(), isalnum(), isspace() */
#include <limits.h>     /* System limits: PATH_MAX */

#define TERMINATOR_VERSION "1.0.0"

/* Core capacity and sizing constants */
#define MAX_TABS 16                  /* Maximum number of simultaneous open tabs */
#define MAX_CMD_LEN 4096             /* Maximum command length in bytes */
#define MAX_PATH_LEN 4096            /* Maximum filesystem path length */
#define MAX_LINE_LEN 1024            /* Maximum length for a single script line */
#define MAX_BUFFER_LINES 2000        /* Scrollback history buffer depth per tab */
#define MAX_HISTORY_TOTAL 10000      /* Global persistent command history storage limit */
#define HISTORY_DISPLAY_COUNT 1000   /* Maximum history items displayed by 'history' command */
#define MAX_JOBS 64                  /* Maximum background jobs tracked per tab */
#define MAX_ARGS 128                 /* Maximum arguments per command line */
#define MAX_PIPES 64                 /* Maximum commands chained in a single pipeline */

/*
 * Execution mode for tab workers:
 *   - MODE_THREAD:  Tabs run as POSIX threads (pthread) sharing address space.
 *   - MODE_PROCESS: Tabs run as independent processes (fork) with isolated address space.
 */
typedef enum {
    MODE_THREAD = 0,
    MODE_PROCESS = 1
} ExecutionMode;

/*
 * Inter-Process Communication (IPC) message types between GUI and Tab Shell workers:
 */
typedef enum {
    MSG_TYPE_OUTPUT = 1,       /* Shell -> GUI: output text to be drawn on screen */
    MSG_TYPE_CWD = 2,          /* Shell -> GUI: working directory updated for tab title */
    MSG_TYPE_KEY = 3,          /* GUI -> Shell: keystroke or hotkey event */
    MSG_TYPE_SIGNAL = 4,       /* GUI -> Shell: POSIX signal (SIGINT, SIGTSTP) */
    MSG_TYPE_EXIT = 5,         /* Shell -> GUI or GUI -> Shell: tab closing notification */
    MSG_TYPE_BUSY = 6          /* Shell -> GUI: tab busy state changed (1=busy, 0=idle) */
} IpcMessageType;

/*
 * IPC Message Packet:
 * Transmitted across UNIX pipes connecting GUI and tab workers.
 */
typedef struct {
    int type;                  /* Message type (IpcMessageType enum) */
    int len;                   /* Data payload length in bytes */
    char data[MAX_CMD_LEN];    /* Payload buffer */
} IpcMessage;

/*
 * Keyboard event codes recognized by Terminator:
 */
typedef enum {
    KEY_CODE_NONE = 0,
    KEY_CODE_CHAR,             /* Standard printable ASCII character */
    KEY_CODE_LEFT,             /* Left arrow: move cursor left */
    KEY_CODE_RIGHT,            /* Right arrow: move cursor right */
    KEY_CODE_UP,               /* Up arrow: history previous */
    KEY_CODE_DOWN,             /* Down arrow: history next */
    KEY_CODE_CTRL_A,           /* Ctrl+A: move cursor to start of line */
    KEY_CODE_CTRL_E,           /* Ctrl+E: move cursor to end of line */
    KEY_CODE_CTRL_U,           /* Ctrl+U: delete from cursor to start of line */
    KEY_CODE_CTRL_K,           /* Ctrl+K: delete from cursor to end of line */
    KEY_CODE_CTRL_W,           /* Ctrl+W: delete word before cursor */
    KEY_CODE_CTRL_L,           /* Ctrl+L: clear screen */
    KEY_CODE_CTRL_D,           /* Ctrl+D: exit / EOF on empty line */
    KEY_CODE_CTRL_C,           /* Ctrl+C: interrupt signal */
    KEY_CODE_CTRL_Z,           /* Ctrl+Z: suspend signal */
    KEY_CODE_TAB,              /* Tab: auto-completion */
    KEY_CODE_CTRL_R,           /* Ctrl+R: reverse history search */
    KEY_CODE_ENTER,            /* Enter: execute line */
    KEY_CODE_BACKSPACE,        /* Backspace: delete character before cursor */
    KEY_CODE_DELETE,           /* Delete: delete character under cursor */
    KEY_CODE_COPY,             /* Copy action (Ctrl+Shift+C) */
    KEY_CODE_PASTE             /* Paste action (Ctrl+Shift+V) */
} TermKeyCode;

/*
 * Key Event representation:
 */
typedef struct {
    TermKeyCode code;          /* Key code identifier */
    char ch;                   /* Character value for KEY_CODE_CHAR */
} TermKeyEvent;

/*
 * -------------------------------------------------------------------------------------
 * Function: term_write (inline helper)
 *
 * Purpose:
 *   Guarantees complete writing of `count` bytes to file descriptor `fd`.
 *   Handles partial writes (write() writing fewer bytes than requested) and
 *   interrupted system calls (EINTR, EAGAIN) by looping until all bytes are sent.
 *
 * Parameters:
 *   @fd:    Target file descriptor.
 *   @buf:   Pointer to source data buffer.
 *   @count: Total number of bytes to write.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static inline void term_write(int fd, const void *buf, size_t count) {
    if (fd < 0 || !buf || count == 0) return;
    size_t total = 0;
    while (total < count) {
        ssize_t n = write(fd, (const char *)buf + total, count - total);
        if (n <= 0) {
            /* If interrupted by signal or temporary EAGAIN, retry */
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break; /* Pipe broken or fatal error: exit loop */
        }
        total += n; /* Advance written count */
    }
}

#endif /* TERMINATOR_H */
