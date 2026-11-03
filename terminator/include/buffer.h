#ifndef BUFFER_H
#define BUFFER_H

#include "terminator.h"

typedef struct {
    char *lines[MAX_BUFFER_LINES];
    int line_count;
    int scroll_offset; /* Lines scrolled up from the bottom (0 = at newest lines) */
    int cursor_col;
    int max_cols;
    int max_rows;
    pthread_mutex_t lock;
} TermBuffer;

void buffer_init(TermBuffer *tb, int cols, int rows);
void buffer_free(TermBuffer *tb);
void buffer_clear(TermBuffer *tb);
void buffer_append_text(TermBuffer *tb, const char *text, int len);
void buffer_append_line(TermBuffer *tb, const char *line);
void buffer_scroll_up(TermBuffer *tb, int lines);
void buffer_scroll_down(TermBuffer *tb, int lines);
void buffer_scroll_to_bottom(TermBuffer *tb);
const char *buffer_get_line(const TermBuffer *tb, int row);

#endif /* BUFFER_H */
