/*
 * ==============================================================================
 * Project:      Terminator - A Custom Linux Terminal & Shell
 * File:         src/buffer.c
 * Purpose:      Terminal screen scrollback buffer and text management.
 * Description:  Maintains a dynamic circular scrollback buffer (up to 2,000 lines)
 *               for each tab. Handles character output, cursor coordinates, line wraps,
 *               tabs, backspaces, newlines, ANSI color escape code filtering, and
 *               mouse-wheel / Shift+PageUp scroll offsets.
 *
 * Concurrency / Viva Concept:
 * - Why do we need a pthread_mutex_t lock inside TermBuffer?
 *   In Thread Mode, the shell worker thread executes commands and appends text output
 *   (via buffer_append_text) asynchronously. Simultaneously, the main GUI thread reads
 *   the buffer during redraw events (XDrawString).
 *   The mutex lock prevents race conditions and data corruption between the writer and reader!
 * ==============================================================================
 */

#include "buffer.h" /* Declarations for TermBuffer, MAX_BUFFER_LINES, and buffer operations */

/*
 * Function:    buffer_init
 * Purpose:     Initializes a freshly allocated terminal scrollback buffer.
 * Parameters:  tb   - Pointer to the TermBuffer structure to initialize.
 *              cols - Initial visible column count (terminal width in characters).
 *              rows - Initial visible row count (terminal height in characters).
 * Returns:     void.
 */
void buffer_init(TermBuffer *tb, int cols, int rows) {
    if (!tb) return; /* Safety check against NULL pointer */

    /* Initialize the POSIX thread mutex used to protect concurrent buffer access */
    pthread_mutex_init(&tb->lock, NULL);

    /* Reset buffer metrics */
    tb->line_count = 0;      /* Current number of populated lines in buffer */
    tb->scroll_offset = 0;   /* 0 means looking at the very bottom (latest output) */
    tb->cursor_col = 0;      /* Horizontal cursor position on current line */
    /* Ensure minimum safe dimensions */
    tb->max_cols = (cols > 20) ? cols : 80; /* Default standard width: 80 columns */
    tb->max_rows = (rows > 5) ? rows : 24;  /* Default standard height: 24 rows */

    /* Initialize all pointer slots in the circular line array to NULL */
    for (int i = 0; i < MAX_BUFFER_LINES; i++) {
        tb->lines[i] = NULL;
    }

    /* Start with one blank line (allocated on heap with calloc to zero out memory) */
    tb->lines[0] = (char *)calloc(MAX_LINE_LEN, 1);
    tb->line_count = 1;
}

/*
 * Function:    buffer_free
 * Purpose:     Frees all allocated heap lines inside the buffer and destroys its mutex.
 * Parameters:  tb - Pointer to the TermBuffer to deallocate.
 * Returns:     void.
 */
void buffer_free(TermBuffer *tb) {
    if (!tb) return;

    /* Acquire lock before freeing underlying memory */
    pthread_mutex_lock(&tb->lock);
    for (int i = 0; i < tb->line_count; i++) {
        if (tb->lines[i]) {
            free(tb->lines[i]); /* Free each dynamically allocated text line */
            tb->lines[i] = NULL;
        }
    }
    tb->line_count = 0;
    pthread_mutex_unlock(&tb->lock);

    /* Destroy the pthread mutex to release OS synchronization resources */
    pthread_mutex_destroy(&tb->lock);
}

/*
 * Function:    buffer_clear
 * Purpose:     Clears all text from the screen buffer (equivalent to Ctrl+L / 'clear').
 * Parameters:  tb - Pointer to the TermBuffer to clear.
 * Returns:     void.
 */
void buffer_clear(TermBuffer *tb) {
    if (!tb) return;

    pthread_mutex_lock(&tb->lock);
    /* Free all existing lines */
    for (int i = 0; i < tb->line_count; i++) {
        if (tb->lines[i]) {
            free(tb->lines[i]);
            tb->lines[i] = NULL;
        }
    }
    /* Allocate one clean empty line */
    tb->lines[0] = (char *)calloc(MAX_LINE_LEN, 1);
    tb->line_count = 1;
    tb->cursor_col = 0;      /* Reset cursor to column 0 */
    tb->scroll_offset = 0;   /* Reset scroll offset */
    pthread_mutex_unlock(&tb->lock);
}

/*
 * Function:    push_newline
 * Purpose:     Appends a new blank line to the bottom of the buffer.
 * Parameters:  tb - Pointer to TermBuffer.
 * Returns:     void.
 * Explanation: Implements circular scrollback: when MAX_BUFFER_LINES (2000) is reached,
 *              the oldest line at index 0 is freed and remaining lines are shifted up by 1.
 */
static void push_newline(TermBuffer *tb) {
    if (tb->line_count >= MAX_BUFFER_LINES) {
        /* Buffer is full: discard the oldest line at the top */
        free(tb->lines[0]);
        for (int i = 0; i < MAX_BUFFER_LINES - 1; i++) {
            tb->lines[i] = tb->lines[i + 1];
        }
        tb->line_count = MAX_BUFFER_LINES - 1;
    }
    /* Allocate a new blank line at the bottom */
    tb->lines[tb->line_count] = (char *)calloc(MAX_LINE_LEN, 1);
    tb->line_count++;
    tb->cursor_col = 0; /* Carriage return: cursor moves to start of new line */
}

/*
 * Function:    buffer_append_line
 * Purpose:     Convenience function to append a full text string followed by a newline.
 * Parameters:  tb   - Pointer to TermBuffer.
 *              line - Null-terminated string to append.
 * Returns:     void.
 */
void buffer_append_line(TermBuffer *tb, const char *line) {
    if (!tb || !line) return;
    buffer_append_text(tb, line, strlen(line));
    buffer_append_text(tb, "\n", 1);
}

/*
 * Function:    buffer_append_text
 * Purpose:     Core text ingestion routine. Parses raw characters coming from pipes.
 * Parameters:  tb   - Pointer to TermBuffer.
 *              text - Array of characters received from process output.
 *              len  - Number of bytes to process.
 * Returns:     void.
 * Explanation: Processes special control characters:
 *              - '\n': moves to a new line
 *              - '\r': returns cursor to column 0
 *              - '\b': backspace (erases previous character)
 *              - '\t': expands to tab stops (4 spaces)
 *              - '\033': filters out ANSI color sequences (\033[...m) so raw codes don't clutter the screen.
 */
void buffer_append_text(TermBuffer *tb, const char *text, int len) {
    if (!tb || !text || len <= 0) return;

    /* Acquire mutex lock: guarantees thread-safety against concurrent GUI redraw */
    pthread_mutex_lock(&tb->lock);

    /* If buffer has no lines, create the first line */
    if (tb->line_count == 0) {
        tb->lines[0] = (char *)calloc(MAX_LINE_LEN, 1);
        tb->line_count = 1;
        tb->cursor_col = 0;
    }

    /* Iterate through every input character */
    for (int i = 0; i < len; i++) {
        char c = text[i];

        /*
         * Filter ANSI Escape Sequences:
         * Commands like 'ls --color' or 'git' output escape codes like \033[31m.
         * We parse and skip them so raw terminal codes don't show up as garbage text.
         */
        if (c == '\033') {
            if (i + 1 < len && text[i + 1] == '[') {
                i += 2;
                /* Skip numeric parameters until the final command character (e.g., 'm') */
                while (i < len && !isalpha((unsigned char)text[i])) {
                    i++;
                }
                continue;
            } else if (i + 1 < len && text[i + 1] == ']') {
                /* OSC sequence e.g. \033]0;title\007: skip until BEL (\007) */
                while (i < len && text[i] != '\007' && text[i] != '\033') {
                    i++;
                }
                continue;
            }
        }

        /* Handle newline '\n' (Line Feed) */
        if (c == '\n') {
            push_newline(tb);
            tb->scroll_offset = 0; /* Auto-scroll to bottom on new output */
        }
        /* Handle carriage return '\r' (move cursor to column 0 without newline) */
        else if (c == '\r') {
            tb->cursor_col = 0;
        }
        /* Handle backspace '\b' */
        else if (c == '\b') {
            if (tb->cursor_col > 0) {
                tb->cursor_col--;
                int cur_line = tb->line_count - 1;
                if (cur_line >= 0 && tb->lines[cur_line]) {
                    tb->lines[cur_line][tb->cursor_col] = ' '; /* Overwrite with space */
                }
            }
        }
        /* Handle tab '\t' (aligns to next 4-column tab stop) */
        else if (c == '\t') {
            int cur_line = tb->line_count - 1;
            char *line = tb->lines[cur_line];
            int spaces = 4 - (tb->cursor_col % 4);
            for (int s = 0; s < spaces; s++) {
                if (tb->cursor_col < MAX_LINE_LEN - 1) {
                    line[tb->cursor_col++] = ' ';
                }
            }
            line[tb->cursor_col] = '\0';
        }
        /* Handle printable characters (ASCII >= 32 or UTF-8 multibyte continuation) */
        else if ((unsigned char)c >= 32 || c < 0) {
            int cur_line = tb->line_count - 1;
            char *line = tb->lines[cur_line];

            /* Auto-wrap line if cursor reaches window boundary */
            if (tb->cursor_col >= MAX_LINE_LEN - 2 || tb->cursor_col >= tb->max_cols) {
                push_newline(tb);
                cur_line = tb->line_count - 1;
                line = tb->lines[cur_line];
            }

            /* Place character into current line at cursor_col */
            line[tb->cursor_col] = c;
            tb->cursor_col++;
            if (tb->cursor_col < MAX_LINE_LEN) {
                line[tb->cursor_col] = '\0'; /* Ensure null-terminated string */
            }
        }
    }

    tb->scroll_offset = 0; /* Automatically snap view to bottom on new text */
    pthread_mutex_unlock(&tb->lock); /* Release mutex lock */
}

/*
 * Function:    buffer_scroll_up
 * Purpose:     Scrolls visible view upwards into historical output.
 * Parameters:  tb    - Pointer to TermBuffer.
 *              lines - Number of lines to scroll up (e.g., 3 for mouse wheel, 10 for PageUp).
 * Returns:     void.
 */
void buffer_scroll_up(TermBuffer *tb, int lines) {
    if (!tb) return;
    pthread_mutex_lock(&tb->lock);
    int max_scroll = tb->line_count - tb->max_rows;
    if (max_scroll < 0) max_scroll = 0;
    tb->scroll_offset += lines;
    if (tb->scroll_offset > max_scroll) {
        tb->scroll_offset = max_scroll; /* Clamp to maximum historical lines */
    }
    pthread_mutex_unlock(&tb->lock);
}

/*
 * Function:    buffer_scroll_down
 * Purpose:     Scrolls visible view downwards back towards latest live prompt.
 * Parameters:  tb    - Pointer to TermBuffer.
 *              lines - Number of lines to scroll down.
 * Returns:     void.
 */
void buffer_scroll_down(TermBuffer *tb, int lines) {
    if (!tb) return;
    pthread_mutex_lock(&tb->lock);
    tb->scroll_offset -= lines;
    if (tb->scroll_offset < 0) {
        tb->scroll_offset = 0; /* 0 is the bottom-most live view */
    }
    pthread_mutex_unlock(&tb->lock);
}

/*
 * Function:    buffer_scroll_to_bottom
 * Purpose:     Instantly snaps view back to live output.
 * Parameters:  tb - Pointer to TermBuffer.
 * Returns:     void.
 */
void buffer_scroll_to_bottom(TermBuffer *tb) {
    if (!tb) return;
    pthread_mutex_lock(&tb->lock);
    tb->scroll_offset = 0;
    pthread_mutex_unlock(&tb->lock);
}

/*
 * Function:    buffer_get_line
 * Purpose:     Retrieves a read-only pointer to the text string at a specific buffer row.
 * Parameters:  tb  - Pointer to TermBuffer.
 *              row - Row index in lines array (0 to line_count-1).
 * Returns:     const char* - Pointer to line string, or empty string "" if out of bounds.
 */
const char *buffer_get_line(const TermBuffer *tb, int row) {
    if (!tb || row < 0 || row >= tb->line_count) return "";
    return tb->lines[row] ? tb->lines[row] : "";
}
