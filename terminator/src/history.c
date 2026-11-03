/*
 * ==============================================================================
 * Project:      Terminator - A Custom Linux Terminal & Shell
 * File:         src/history.c
 * Purpose:      Shared command history subsystem and Ctrl+R search engine.
 * Description:  Maintains a 10,000-command ring buffer that is shared across all
 *               tabs. In Thread Mode, it uses a heap buffer protected by a thread semaphore.
 *               In Process Mode, it uses POSIX Shared Memory (mmap) protected by a
 *               process-shared semaphore (sem_init with pshared = 1).
 *               Also persists history to ~/.terminator_history and performs Ctrl+R
 *               lookups using exact matching followed by Longest Common Substring (LCS).
 *
 * Operating System / Viva Concepts:
 * 1. What is mmap(MAP_SHARED | MAP_ANONYMOUS)?
 *    - Creates an anonymous shared memory segment in RAM not backed by any file.
 *    - Child processes created via fork() inherit this shared memory map, allowing
 *      all forked tab processes to read and write to the exact same history buffer!
 * 2. Why use a POSIX Semaphore (sem_t) instead of a mutex?
 *    - sem_init(&sem, 1, 1) sets the second argument (pshared) to 1.
 *      This tells the Linux kernel that the semaphore is shared across different processes!
 *      A standard pthread_mutex by default only works within the same process.
 * 3. What is the Ctrl+R algorithm?
 *    - First checks for exact string matches from most recent to oldest.
 *    - If no exact match exists, it computes the Longest Common Substring (LCS) using
 *      Dynamic Programming in O(len1 * len2) time.
 *    - If the longest common substring length > 2, it returns all commands sharing that maximum substring!
 * ==============================================================================
 */

#include "history.h" /* Declarations for SharedHistory, MAX_HISTORY_TOTAL, and history functions */

/* Global pointer to the active SharedHistory structure (either heap or mmap-backed) */
static SharedHistory *shm_hist = NULL;

/* Current execution mode (MODE_THREAD or MODE_PROCESS) */
static ExecutionMode hist_mode = MODE_THREAD;

/* Path to the persistent history file (~/.terminator_history) */
static char hist_filepath[MAX_PATH_LEN] = {0};

/*
 * Function:    get_history_filepath
 * Purpose:     Constructs the full path to ~/.terminator_history using $HOME.
 * Parameters:  dest - Character buffer to store the constructed file path.
 *              size - Maximum size of the destination buffer.
 * Returns:     void.
 */
static void get_history_filepath(char *dest, size_t size) {
    const char *home = getenv("HOME"); /* Retrieve home directory from environment */
    if (home) {
        snprintf(dest, size, "%s/.terminator_history", home);
    } else {
        snprintf(dest, size, ".terminator_history");
    }
}

/*
 * Function:    longest_common_substring
 * Purpose:     Computes the length of the Longest Common Substring between two strings
 *              using space-optimized Dynamic Programming.
 * Parameters:  s1 - First string (e.g. historical command).
 *              s2 - Second string (e.g. search query).
 * Returns:     int - Length of the longest common continuous substring.
 *
 * Algorithm Explanation:
 * - dp[j] stores the length of the common suffix of substrings s1[0..i-1] and s2[0..j-1].
 * - If s1[i-1] == s2[j-1], dp[j] = dp[j-1] + 1.
 * - Otherwise, dp[j] = 0.
 * - We traverse backwards (j from len2 down to 1) to use O(len2) space instead of O(len1 * len2).
 */
static int longest_common_substring(const char *s1, const char *s2) {
    if (!s1 || !s2) return 0;
    int len1 = strlen(s1);
    int len2 = strlen(s2);
    if (len1 == 0 || len2 == 0) return 0;

    int max_len = 0;
    /* Allocate 1D DP table of size (len2 + 1) initialized to 0 */
    int *dp = (int *)calloc(len2 + 1, sizeof(int));
    if (!dp) return 0;

    for (int i = 1; i <= len1; i++) {
        for (int j = len2; j >= 1; j--) {
            if (s1[i - 1] == s2[j - 1]) {
                dp[j] = dp[j - 1] + 1;
                if (dp[j] > max_len) {
                    max_len = dp[j]; /* Track the global maximum length found */
                }
            } else {
                dp[j] = 0; /* Reset since substrings must be continuous */
            }
        }
    }

    free(dp);        /* Free temporary DP memory */
    return max_len;  /* Return maximum substring length */
}

/*
 * Function:    history_load_from_file
 * Purpose:     Loads previously persisted commands from ~/.terminator_history into memory on startup.
 * Parameters:  void.
 * Returns:     void.
 */
static void history_load_from_file(void) {
    if (!shm_hist) return;
    FILE *fp = fopen(hist_filepath, "r");
    if (!fp) return; /* File doesn't exist yet on first run; return cleanly */

    char line[MAX_CMD_LEN];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newlines and carriage returns */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            len--;
        }
        if (len == 0) continue; /* Skip blank lines */

        /* Add to in-memory history ring buffer */
        if (shm_hist->count < MAX_HISTORY_TOTAL) {
            strncpy(shm_hist->commands[shm_hist->count], line, MAX_CMD_LEN - 1);
            shm_hist->count++;
        } else {
            /* Ring buffer full: shift all entries left to make room for newest command */
            for (int i = 0; i < MAX_HISTORY_TOTAL - 1; i++) {
                memcpy(shm_hist->commands[i], shm_hist->commands[i + 1], MAX_CMD_LEN);
            }
            strncpy(shm_hist->commands[MAX_HISTORY_TOTAL - 1], line, MAX_CMD_LEN - 1);
        }
    }
    fclose(fp);
}

/*
 * Function:    history_init
 * Purpose:     Initializes the shared history memory and synchronization semaphore.
 * Parameters:  mode - ExecutionMode (MODE_THREAD or MODE_PROCESS).
 * Returns:     void.
 */
void history_init(ExecutionMode mode) {
    hist_mode = mode;
    get_history_filepath(hist_filepath, sizeof(hist_filepath));

    if (mode == MODE_PROCESS) {
        /*
         * Process Mode: Shared Memory Setup
         * - mmap() creates a memory mapping in the process address space.
         * - MAP_SHARED: Changes are shared across all forked child processes.
         * - MAP_ANONYMOUS: Backed by RAM, not by an on-disk file.
         * - PROT_READ | PROT_WRITE: Read and write permissions.
         */
        shm_hist = (SharedHistory *)mmap(NULL, sizeof(SharedHistory),
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (shm_hist == MAP_FAILED) {
            perror("mmap shared history failed");
            shm_hist = (SharedHistory *)calloc(1, sizeof(SharedHistory));
            sem_init(&shm_hist->sem, 0, 1);
        } else {
            memset(shm_hist, 0, sizeof(SharedHistory));
            /*
             * sem_init(&sem, pshared, value):
             * - pshared = 1: Semaphore is shared across different processes!
             * - value = 1: Binary semaphore (acts like a mutex lock, initially unlocked).
             */
            sem_init(&shm_hist->sem, 1, 1);
        }
    } else {
        /*
         * Thread Mode:
         * Standard heap allocation with calloc().
         * pshared = 0 indicates semaphore is shared only between threads of the same process.
         */
        shm_hist = (SharedHistory *)calloc(1, sizeof(SharedHistory));
        sem_init(&shm_hist->sem, 0, 1);
    }

    /* Load saved historical commands from ~/.terminator_history file */
    history_load_from_file();
}

/*
 * Function:    history_cleanup
 * Purpose:     Releases shared memory and destroys the history semaphore on shutdown.
 * Parameters:  void.
 * Returns:     void.
 */
void history_cleanup(void) {
    if (!shm_hist) return;
    sem_destroy(&shm_hist->sem); /* Destroy semaphore */
    if (hist_mode == MODE_PROCESS) {
        /* munmap releases mapped shared memory pages back to OS kernel */
        munmap(shm_hist, sizeof(SharedHistory));
    } else {
        free(shm_hist); /* Free heap memory */
    }
    shm_hist = NULL;
}

/*
 * Function:    history_add
 * Purpose:     Adds a new command string to the shared history and appends it to disk.
 * Parameters:  cmd - The command line string to record.
 * Returns:     void.
 */
void history_add(const char *cmd) {
    if (!shm_hist || !cmd || strlen(cmd) == 0) return;

    /* Acquire semaphore lock (decrements semaphore; blocks if another tab is writing) */
    sem_wait(&shm_hist->sem);

    /* Deduplication: do not add command if it is identical to the immediately preceding one */
    if (shm_hist->count > 0 && strcmp(shm_hist->commands[shm_hist->count - 1], cmd) == 0) {
        sem_post(&shm_hist->sem); /* Release semaphore */
        return;
    }

    /* Insert into array */
    if (shm_hist->count < MAX_HISTORY_TOTAL) {
        strncpy(shm_hist->commands[shm_hist->count], cmd, MAX_CMD_LEN - 1);
        shm_hist->commands[shm_hist->count][MAX_CMD_LEN - 1] = '\0';
        shm_hist->count++;
    } else {
        /* Array is full (10,000 commands): shift oldest off the top */
        for (int i = 0; i < MAX_HISTORY_TOTAL - 1; i++) {
            memcpy(shm_hist->commands[i], shm_hist->commands[i + 1], MAX_CMD_LEN);
        }
        strncpy(shm_hist->commands[MAX_HISTORY_TOTAL - 1], cmd, MAX_CMD_LEN - 1);
        shm_hist->commands[MAX_HISTORY_TOTAL - 1][MAX_CMD_LEN - 1] = '\0';
    }

    /* Append the command to ~/.terminator_history file */
    FILE *fp = fopen(hist_filepath, "a");
    if (fp) {
        fprintf(fp, "%s\n", cmd);
        fclose(fp);
    }

    /* Release semaphore lock (increments semaphore, unblocking waiting processes/threads) */
    sem_post(&shm_hist->sem);
}

/*
 * Function:    history_count
 * Purpose:     Returns the total number of commands stored in history.
 * Parameters:  void.
 * Returns:     int - Count of stored commands.
 */
int history_count(void) {
    if (!shm_hist) return 0;
    sem_wait(&shm_hist->sem);
    int c = shm_hist->count;
    sem_post(&shm_hist->sem);
    return c;
}

/*
 * Function:    history_get
 * Purpose:     Fetches the command at a specific historical index.
 * Parameters:  index   - 0-indexed position in commands array.
 *              out     - Buffer to copy the command into.
 *              max_len - Maximum size of output buffer.
 * Returns:     int     - Returns 1 if successfully retrieved, 0 if index out of bounds.
 */
int history_get(int index, char *out, int max_len) {
    if (!shm_hist || !out || max_len <= 0) return 0;
    sem_wait(&shm_hist->sem);
    int res = 0;
    if (index >= 0 && index < shm_hist->count) {
        strncpy(out, shm_hist->commands[index], max_len - 1);
        out[max_len - 1] = '\0';
        res = 1;
    }
    sem_post(&shm_hist->sem);
    return res;
}

/*
 * Function:    history_print_recent
 * Purpose:     Formats the most recent n commands into a numbered list string.
 * Parameters:  n           - Number of recent entries to format (default 10).
 *              out_buf     - Destination string buffer.
 *              max_buf_len - Capacity of destination buffer.
 * Returns:     void.
 */
void history_print_recent(int n, char *out_buf, int max_buf_len) {
    if (!shm_hist || !out_buf || max_buf_len <= 0) return;
    out_buf[0] = '\0';

    sem_wait(&shm_hist->sem);
    if (n <= 0) n = HISTORY_DISPLAY_COUNT;
    int start = shm_hist->count - n;
    if (start < 0) start = 0;

    int offset = 0;
    for (int i = start; i < shm_hist->count; i++) {
        int written = snprintf(out_buf + offset, max_buf_len - offset, "%5d  %s\n",
                               i + 1, shm_hist->commands[i]);
        if (written > 0) {
            offset += written;
            if (offset >= max_buf_len - 1) break;
        }
    }
    sem_post(&shm_hist->sem);
}

/*
 * Function:    history_search_ctrl_r
 * Purpose:     Implements Ctrl+R reverse interactive history search.
 * Parameters:  term        - The search token typed by user.
 *              out_buf     - Destination buffer for matching commands.
 *              max_buf_len - Capacity of output buffer.
 * Returns:     void.
 *
 * Algorithm Strategy:
 * 1. Exact Match: Searches backwards from newest to oldest command for an identical match.
 * 2. Longest Common Substring (LCS): Computes maximum substring overlap. If max_lcs > 2,
 *    returns all historical commands sharing that longest common substring without duplicates.
 * 3. No match message if no meaningful overlap is found.
 */
void history_search_ctrl_r(const char *term, char *out_buf, int max_buf_len) {
    if (!shm_hist || !term || !out_buf || max_buf_len <= 0) return;
    out_buf[0] = '\0';

    if (strlen(term) == 0) {
        snprintf(out_buf, max_buf_len, "No match for search term in history\n");
        return;
    }

    sem_wait(&shm_hist->sem);

    /* 1. Exact match check (from most recent to oldest) */
    for (int i = shm_hist->count - 1; i >= 0; i--) {
        if (strcmp(shm_hist->commands[i], term) == 0) {
            snprintf(out_buf, max_buf_len, "%s\n", shm_hist->commands[i]);
            sem_post(&shm_hist->sem);
            return;
        }
    }

    /* 2. Longest Common Substring check across all historical commands */
    int max_lcs = 0;
    for (int i = 0; i < shm_hist->count; i++) {
        int lcs = longest_common_substring(shm_hist->commands[i], term);
        if (lcs > max_lcs) {
            max_lcs = lcs;
        }
    }

    /* If we found a significant substring match (> 2 characters) */
    if (max_lcs > 2) {
        int offset = 0;
        char seen[64][MAX_CMD_LEN];
        int seen_count = 0;

        /* Collect all commands that share this max_lcs length */
        for (int i = 0; i < shm_hist->count; i++) {
            if (longest_common_substring(shm_hist->commands[i], term) == max_lcs) {
                /* Deduplication check: do not print duplicate commands */
                int already_seen = 0;
                for (int s = 0; s < seen_count; s++) {
                    if (strcmp(seen[s], shm_hist->commands[i]) == 0) {
                        already_seen = 1;
                        break;
                    }
                }
                if (!already_seen && seen_count < 64) {
                    strncpy(seen[seen_count++], shm_hist->commands[i], MAX_CMD_LEN - 1);
                    int written = snprintf(out_buf + offset, max_buf_len - offset, "%s\n",
                                           shm_hist->commands[i]);
                    if (written > 0) {
                        offset += written;
                        if (offset >= max_buf_len - 1) break;
                    }
                }
            }
        }
        sem_post(&shm_hist->sem);
        return;
    }

    /* 3. No match found */
    snprintf(out_buf, max_buf_len, "No match for search term in history\n");
    sem_post(&shm_hist->sem);
}

/*
 * Function:    history_clear
 * Purpose:     Resets the count of commands in memory to 0 (used for test isolation).
 * Parameters:  void.
 * Returns:     void.
 */
void history_clear(void) {
    if (!shm_hist) return;
    sem_wait(&shm_hist->sem);
    shm_hist->count = 0;
    sem_post(&shm_hist->sem);
}
