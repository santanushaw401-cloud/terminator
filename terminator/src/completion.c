/*
 * ==============================================================================
 * Project:      Terminator - A Custom Linux Terminal & Shell
 * File:         src/completion.c
 * Purpose:      Tab autocompletion and filename prefix matching.
 * Description:  Scans directories using POSIX directory streams (opendir, readdir),
 *               filters files matching the typed prefix, computes the Longest Common
 *               Prefix (LCP) when multiple files match, and formats candidate lists.
 *
 * Algorithm & Viva Explanations:
 * 1. How does Tab completion find the active word?
 *    - Scans backwards from cursor_pos until hitting whitespace (' ' or '\t').
 *    - Splits the word into directory part (e.g., "src/") and filename prefix (e.g., "mai").
 * 2. What is the Longest Common Prefix (LCP) algorithm?
 *    - Given matches ["main.c", "main.o", "maintain.c"]:
 *      Compares character by character across all candidate strings at index 0, 1, 2...
 *      Stops at the first mismatch. Here, LCP is "main".
 *      If the user presses Tab on "m", it auto-expands to "main"!
 * 3. Why use opendir() and readdir()?
 *    - opendir() opens a directory stream corresponding to the directory path.
 *    - readdir() returns a pointer to a struct dirent representing the next directory entry.
 *    - closedir() closes the directory stream and frees associated file descriptors.
 * ==============================================================================
 */

#include "completion.h" /* Declarations for CompletionResult and completion prototypes */

/*
 * Function:    longest_common_prefix
 * Purpose:     Finds the maximum common starting substring shared by an array of strings.
 * Parameters:  matches - Array of matching filename strings.
 *              count   - Total number of matches in the array.
 *              out     - Buffer where the common prefix string will be written.
 *              max_len - Maximum capacity of the output buffer.
 * Returns:     const char* - Pointer to the populated out buffer.
 */
const char *longest_common_prefix(char matches[][MAX_FILENAME_LEN], int count, char *out, int max_len) {
    if (!out || max_len <= 0) return "";
    out[0] = '\0';
    if (count <= 0) return out; /* No matches -> empty prefix */

    int prefix_len = 0;

    /* Loop column by column through the characters of matches[0] */
    while (1) {
        char c = matches[0][prefix_len];
        if (c == '\0') break; /* Reached end of the first candidate string */

        int all_match = 1;
        /* Verify if every other matching candidate has the exact same character at prefix_len */
        for (int i = 1; i < count; i++) {
            if (matches[i][prefix_len] != c) {
                all_match = 0; /* Character mismatch found */
                break;
            }
        }
        if (!all_match) break; /* Stop at first divergence */

        /* Append character to output prefix if within bounds */
        if (prefix_len < max_len - 1) {
            out[prefix_len] = c;
            prefix_len++;
        } else {
            break;
        }
    }
    out[prefix_len] = '\0'; /* Null-terminate result */
    return out;
}

/*
 * Function:    cmp_str
 * Purpose:     Comparator function passed to qsort() for alphabetical filename sorting.
 * Parameters:  a, b - Pointers to strings being compared.
 * Returns:     int  - Negative if a < b, 0 if a == b, positive if a > b.
 */
static int cmp_str(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/*
 * Function:    complete_word
 * Purpose:     Performs tab completion for the word under the user's cursor.
 * Parameters:  line       - The full raw command line typed so far.
 *              cursor_pos - Index of cursor in line.
 *              cwd        - Current working directory of the tab.
 *              res        - Pointer to CompletionResult structure to populate.
 * Returns:     int        - Number of matching files found (0 if none).
 */
int complete_word(const char *line, int cursor_pos, const char *cwd, CompletionResult *res) {
    if (!line || !res) return 0;
    memset(res, 0, sizeof(CompletionResult)); /* Clear result structure */

    /* Clamp cursor position within valid string bounds */
    if (cursor_pos < 0) cursor_pos = 0;
    if (cursor_pos > (int)strlen(line)) cursor_pos = strlen(line);

    /*
     * 1. Locate word boundaries:
     * Walk backwards from cursor position until we hit whitespace or the start of the line.
     */
    int start = cursor_pos;
    while (start > 0 && line[start - 1] != ' ' && line[start - 1] != '\t') {
        start--;
    }

    res->word_start = start;      /* Offset where the word starts in line */
    res->word_end = cursor_pos;   /* Offset where cursor is currently located */

    /* Copy word token into temporary buffer */
    char word[MAX_PATH_LEN] = {0};
    int word_len = cursor_pos - start;
    if (word_len >= MAX_PATH_LEN) word_len = MAX_PATH_LEN - 1;
    strncpy(word, line + start, word_len);
    word[word_len] = '\0';

    /*
     * 2. Separate directory component and filename prefix:
     * If word is "src/mai", dir_part is "src/" and file_prefix is "mai".
     * If word is "mai", dir_part is "" and file_prefix is "mai".
     */
    char dir_to_open[MAX_PATH_LEN] = {0};
    char file_prefix[MAX_PATH_LEN] = {0};

    char *last_slash = strrchr(word, '/'); /* Look for the rightmost '/' */
    if (last_slash) {
        int dir_len = last_slash - word + 1; /* Include the trailing slash */
        strncpy(res->dir_part, word, dir_len);
        res->dir_part[dir_len] = '\0';

        strncpy(file_prefix, last_slash + 1, sizeof(file_prefix) - 1);

        /* Construct absolute directory path to scan */
        if (res->dir_part[0] == '/') {
            strncpy(dir_to_open, res->dir_part, sizeof(dir_to_open) - 1);
        } else {
            snprintf(dir_to_open, sizeof(dir_to_open), "%s/%s", cwd ? cwd : ".", res->dir_part);
        }
    } else {
        res->dir_part[0] = '\0';
        strncpy(file_prefix, word, sizeof(file_prefix) - 1);
        strncpy(dir_to_open, cwd ? cwd : ".", sizeof(dir_to_open) - 1);
    }

    /*
     * 3. Open directory stream:
     * opendir() opens the specified directory so we can iterate over entries.
     */
    DIR *d = opendir(dir_to_open);
    if (!d) return 0; /* Cannot open directory (does not exist or no permission) */

    struct dirent *entry;
    int prefix_len = strlen(file_prefix);

    /* Iterate through every file/directory entry inside dir_to_open */
    while ((entry = readdir(d)) != NULL) {
        /* Ignore self ('.') and parent ('..') directories */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /* Check if file starts with the typed prefix */
        if (prefix_len == 0 || strncmp(entry->d_name, file_prefix, prefix_len) == 0) {
            if (res->match_count < MAX_COMPLETION_MATCHES) {
                snprintf(res->matches[res->match_count], sizeof(res->matches[0]), "%s", entry->d_name);
                res->match_count++;
            }
        }
    }
    closedir(d); /* Close directory stream */

    if (res->match_count == 0) return 0; /* No matches found */

    /*
     * 4. Sort matches alphabetically:
     * Uses standard C library qsort() with our string comparison function.
     */
    qsort(res->matches, res->match_count, sizeof(res->matches[0]), cmp_str);

    /*
     * 5. Compute completion string:
     * - If exactly 1 match: common prefix is that entire filename.
     * - If multiple matches: compute the Longest Common Prefix (LCP).
     */
    if (res->match_count == 1) {
        snprintf(res->common_prefix, sizeof(res->common_prefix), "%s", res->matches[0]);
    } else {
        longest_common_prefix(res->matches, res->match_count, res->common_prefix, sizeof(res->common_prefix));
    }

    return res->match_count; /* Return total matches found */
}
