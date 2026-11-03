/*
 * =====================================================================================
 * File: src/gui.c
 * Project: Terminator - Custom Tabbed Terminal Emulator & Shell
 *
 * Educational Purpose & Viva Guide:
 * This file implements the entire Graphical User Interface (GUI) subsystem using
 * the X11 (Xlib) windowing protocol. It handles:
 *   1. X11 Display connection and Window lifecycle (creation, drawing, resizing, destruction).
 *   2. Monospace font loading, character cell metrics computation, and text rendering.
 *   3. Custom Tab Bar rendering with dynamic width calculation, tab ID recycling, and
 *      individual red 'x' close buttons per tab.
 *   4. Keyboard event translation: converts raw X11 KeySyms into portable TermKeyEvents
 *      honoring custom ~/.terminatorrc keybindings.
 *   5. Mouse interaction: tab switching, tab closure ('x' button / middle click), '+' button,
 *      and mouse wheel scrollback navigation.
 *   6. Clipboard integration: X11 ICCCM selection protocol (CLIPBOARD and PRIMARY atoms).
 *   7. Central Non-Blocking Event Multiplexing Loop: uses select() to concurrently monitor
 *      both the X11 socket and IPC output pipes from all running background tab workers.
 *
 * System Calls & Libraries Used:
 *   - <X11/Xlib.h>, <X11/Xutil.h>, <X11/Xatom.h>, <X11/keysym.h>:
 *       XOpenDisplay(), XCreateSimpleWindow(), XSelectInput(), XCreateGC(), XLoadQueryFont(),
 *       XDrawString(), XFillRectangle(), XNextEvent(), XPending(), XInternAtom(), etc.
 *   - <sys/select.h>: select(), FD_SET(), FD_ZERO(), FD_ISSET() for non-blocking I/O multiplexing.
 *   - <pthread.h>: pthread_mutex_lock(), pthread_mutex_unlock(), pthread_join() for thread safety.
 *   - <unistd.h>: read(), write(), close() for IPC pipe I/O.
 *   - <stdlib.h>: calloc(), free(), getenv() for dynamic memory and environment lookup.
 * =====================================================================================
 */

#include "gui.h" /* Header declaring GuiApp, gui_init(), gui_run(), gui_cleanup(), etc. */

/*
 * -------------------------------------------------------------------------------------
 * Function: get_color (static helper)
 *
 * Purpose:
 *   Allocates a color on the X server display given its hex representation (e.g. "#1e1e1e")
 *   or standard name. If allocation fails (e.g., colormap exhausted), it safely returns
 *   a supplied fallback pixel value.
 *
 * Parameters:
 *   @dpy:        Pointer to active X11 Display structure.
 *   @color_name: String representing the color (e.g., "#1e1e1e", "#d4d4d4").
 *   @fallback:   Pixel value to return if XAllocNamedColor() fails.
 *
 * Returns:
 *   unsigned long: Pixel value used by the X server for drawing routines.
 * -------------------------------------------------------------------------------------
 */
static unsigned long get_color(Display *dpy, const char *color_name, unsigned long fallback) {
    XColor col, exact;
    /* DefaultColormap(): Retrieves the default color mapping table for the screen */
    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));

    /* XAllocNamedColor(): Allocates a read-only color cell matching color_name */
    if (XAllocNamedColor(dpy, cmap, color_name, &col, &exact)) {
        return col.pixel; /* Successfully allocated: return hardware pixel value */
    }
    return fallback;      /* Allocation failed: return fallback pixel value */
}

/*
 * -------------------------------------------------------------------------------------
 * Function: shorten_path (static helper)
 *
 * Purpose:
 *   Formats directory paths for compact display in tab titles.
 *   Replaces the user's home directory prefix (e.g., "/home/santanu") with "~".
 *
 * Parameters:
 *   @path:     Full absolute directory path (e.g., "/home/santanu/terminator").
 *   @out:      Output buffer to receive shortened path (e.g., "~/terminator").
 *   @out_size: Capacity of output buffer.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static void shorten_path(const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size == 0) return;
    const char *home = getenv("HOME"); /* Read $HOME environment variable */

    /* If path starts with $HOME, substitute '~' */
    if (home && strncmp(path, home, strlen(home)) == 0) {
        snprintf(out, out_size, "~%s", path + strlen(home));
    } else {
        snprintf(out, out_size, "%s", path);
    }
}

/*
 * -------------------------------------------------------------------------------------
 * Function: gui_init
 *
 * Purpose:
 *   Initializes the X11 Graphical User Interface subsystem:
 *     1. Loads keybinding configuration from ~/.terminatorrc.
 *     2. Establishes connection to the X11 server via XOpenDisplay().
 *     3. Loads a monospace font and calculates font cell metrics (width, height, ascent).
 *     4. Allocates UI theme colors (background, foreground, tab bar, red close cross).
 *     5. Creates the top-level application window via XCreateSimpleWindow().
 *     6. Registers Window Manager protocols (WM_DELETE_WINDOW for clean close button handling).
 *     7. Sets up IPC Atoms for X11 clipboard integration.
 *     8. Configures event masks and creates Graphics Context (GC).
 *     9. Spawns the initial default shell tab.
 *
 * Parameters:
 *   @app:  Pointer to the GuiApp application state structure.
 *   @mode: ExecutionMode enum: MODE_THREAD (pthread workers) or MODE_PROCESS (fork workers).
 *
 * Returns:
 *   int: 0 on success, -1 if X11 display cannot be opened or font loading fails.
 * -------------------------------------------------------------------------------------
 */
int gui_init(GuiApp *app, ExecutionMode mode) {
    if (!app) return -1;
    memset(app, 0, sizeof(GuiApp)); /* Zero out entire GuiApp state structure */
    app->mode = mode;               /* Store execution mode (thread vs process) */

    /* 1. Load user configuration from ~/.terminatorrc */
    config_init_defaults(&app->config);
    config_load_file(&app->config, NULL);

    /* 2. Connect to X11 Server using DISPLAY environment variable */
    app->display = XOpenDisplay(NULL);
    if (!app->display) {
        /* XOpenDisplay returns NULL if no X server is running or DISPLAY is unset */
        fprintf(stderr, "terminator: cannot open X11 display. Please verify that an X server is running and DISPLAY is set.\n");
        return -1;
    }

    app->screen = DefaultScreen(app->display);               /* Get default screen index */
    Window root = RootWindow(app->display, app->screen);      /* Get root (desktop) window */

    /* 3. Try to load standard X11 monospace fonts in order of preference */
    const char *font_names[] = {
        "-misc-fixed-medium-r-normal--15-140-75-75-c-90-iso8859-1",
        "9x15",
        "fixed",
        "-*-courier-medium-r-*-*-14-*-*-*-*-*-*-*",
        NULL
    };

    app->font = NULL;
    for (int i = 0; font_names[i]; i++) {
        /* XLoadQueryFont(): Requests font from X server and inspects font bounds */
        app->font = XLoadQueryFont(app->display, font_names[i]);
        if (app->font) break; /* Found a working monospace font */
    }

    if (!app->font) {
        fprintf(stderr, "terminator: failed to load any fallback monospace font\n");
        XCloseDisplay(app->display);
        return -1;
    }

    /* Compute character cell dimensions for grid layout */
    app->char_width = app->font->max_bounds.width;               /* Fixed character width in pixels */
    app->char_height = app->font->ascent + app->font->descent;   /* Total character cell height */
    app->font_ascent = app->font->ascent;                       /* Baseline offset from cell top */
    app->tab_bar_height = app->char_height + 12;                /* Height of top tab navigation bar */

    /* Initial window dimensions */
    app->win_width = 850;
    app->win_height = 550;

    /* 4. Allocate theme colors using hex codes with hardware fallbacks */
    app->col_bg = get_color(app->display, "#1e1e1e", BlackPixel(app->display, app->screen));             /* Dark gray bg */
    app->col_fg = get_color(app->display, "#d4d4d4", WhitePixel(app->display, app->screen));             /* Light gray text */
    app->col_tab_bg = get_color(app->display, "#2d2d2d", BlackPixel(app->display, app->screen));         /* Tab bar background */
    app->col_tab_active = get_color(app->display, "#3e3e42", WhitePixel(app->display, app->screen));     /* Active tab pill */
    app->col_tab_text = get_color(app->display, "#969696", WhitePixel(app->display, app->screen));       /* Inactive tab text */
    app->col_tab_active_text = get_color(app->display, "#ffffff", WhitePixel(app->display, app->screen));/* Active tab text */
    app->col_tab_close = get_color(app->display, "#e06c75", WhitePixel(app->display, app->screen));      /* Coral red 'x' close */
    app->col_border = get_color(app->display, "#3f3f46", BlackPixel(app->display, app->screen));         /* Border line */

    /* 5. Create top-level application window */
    app->window = XCreateSimpleWindow(
        app->display, root,
        100, 100, app->win_width, app->win_height,
        1, app->col_border, app->col_bg
    );

    /* Set title bar text */
    XStoreName(app->display, app->window, "Terminator");

    /* 6. Intercept window manager close button (the 'X' in window title bar) */
    app->wm_delete_window = XInternAtom(app->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app->display, app->window, &app->wm_delete_window, 1);

    /* 7. Setup Atoms for system clipboard communication (ICCCM standard) */
    app->clipboard_atom = XInternAtom(app->display, "CLIPBOARD", False);
    app->targets_atom = XInternAtom(app->display, "TARGETS", False);
    app->utf8_string_atom = XInternAtom(app->display, "UTF8_STRING", False);

    /* 8. Select input events our application is interested in receiving */
    XSelectInput(app->display, app->window,
                 ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);

    /* Create Graphics Context (GC) used for drawing operations */
    app->gc = XCreateGC(app->display, app->window, 0, NULL);
    XSetFont(app->display, app->gc, app->font->fid); /* Attach font to GC */

    /* 9. Map window on screen (make it visible) and flush display queue */
    XMapWindow(app->display, app->window);
    XFlush(app->display);

    app->running = 1;      /* Flag: application main loop is running */
    app->num_tabs = 0;     /* Initially 0 tabs */
    app->active_tab = 0;   /* First tab index is 0 */

    /* 10. Open the initial default tab */
    gui_add_tab(app);

    return 0; /* GUI initialized successfully */
}

/*
 * -------------------------------------------------------------------------------------
 * Function: get_lowest_available_tab_id (static helper)
 *
 * Purpose:
 *   Finds the smallest positive integer ID (1, 2, 3, ...) not currently assigned to
 *   any open tab.
 *   This ensures tab numbering reuses recycled IDs predictably:
 *   For example, if tabs 2 and 3 are open, the next opened tab receives ID 1,
 *   then 4, then 5, etc.
 *
 * Parameters:
 *   @app: Pointer to GuiApp structure.
 *
 * Returns:
 *   int: Lowest available positive integer ID.
 * -------------------------------------------------------------------------------------
 */
static int get_lowest_available_tab_id(GuiApp *app) {
    for (int candidate = 1; candidate <= MAX_TABS + 1; candidate++) {
        int in_use = 0;
        for (int i = 0; i < app->num_tabs; i++) {
            if (app->tabs[i] && app->tabs[i]->id == candidate) {
                in_use = 1; /* Candidate ID is already assigned to an open tab */
                break;
            }
        }
        if (!in_use) {
            return candidate; /* Found smallest unused ID */
        }
    }
    return app->num_tabs + 1; /* Fallback */
}

/*
 * -------------------------------------------------------------------------------------
 * Function: gui_cleanup
 *
 * Purpose:
 *   Releases all allocated resources when terminating the application:
 *     - Closes and frees all running tabs and their text scrollback buffers.
 *     - Frees X11 font, Graphics Context (GC), Window, and closes the Display connection.
 *
 * Parameters:
 *   @app: Pointer to GuiApp structure.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
void gui_cleanup(GuiApp *app) {
    if (!app || !app->display) return;

    /* 1. Gracefully terminate and free each tab */
    for (int i = 0; i < app->num_tabs; i++) {
        if (app->tabs[i]) {
            shell_tab_close(app->tabs[i]); /* Closes IPC pipes, joins thread or signals child */
            free(app->tabs[i]);           /* Free heap memory of TabState */
            app->tabs[i] = NULL;
        }
        if (app->buffers[i]) {
            buffer_free(app->buffers[i]);  /* Free buffer string memory */
            free(app->buffers[i]);         /* Free heap memory of TermBuffer */
            app->buffers[i] = NULL;
        }
    }

    /* 2. Free X11 graphical resources */
    if (app->font) XFreeFont(app->display, app->font);           /* Free loaded font */
    if (app->gc) XFreeGC(app->display, app->gc);                 /* Free Graphics Context */
    if (app->window) XDestroyWindow(app->display, app->window);  /* Destroy X window */
    XCloseDisplay(app->display);                                 /* Close connection to X server */
    app->display = NULL;
}

/*
 * -------------------------------------------------------------------------------------
 * Function: gui_add_tab
 *
 * Purpose:
 *   Creates and launches a new terminal tab:
 *     1. Allocates heap memory for TermBuffer and TabState.
 *     2. Computes the lowest unused tab ID to recycle IDs cleanly.
 *     3. Initializes the terminal buffer dimensions (columns and rows).
 *     4. Initializes shell state (working directory, jobs, flags).
 *     5. Spawns the background worker (pthread or child process) via shell_tab_start().
 *     6. Switches focus to the newly created tab.
 *
 * Parameters:
 *   @app: Pointer to GuiApp structure.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
void gui_add_tab(GuiApp *app) {
    /* Prevent exceeding maximum allowed tabs */
    if (!app || app->num_tabs >= MAX_TABS) return;

    int idx = app->num_tabs;
    /* Compute grid columns and rows based on window dimensions and font metrics */
    int cols = app->win_width / app->char_width;
    int rows = (app->win_height - app->tab_bar_height) / app->char_height;

    /* Determine lowest available tab ID */
    int new_id = get_lowest_available_tab_id(app);

    /* Allocate TermBuffer and TabState on heap to avoid stack overflow */
    app->buffers[idx] = (TermBuffer *)calloc(1, sizeof(TermBuffer));
    app->tabs[idx] = (TabState *)calloc(1, sizeof(TabState));
    if (!app->buffers[idx] || !app->tabs[idx]) return;

    /* Initialize terminal buffer and shell worker */
    buffer_init(app->buffers[idx], cols, rows);
    shell_tab_init(app->tabs[idx], new_id, app->mode);
    shell_tab_start(app->tabs[idx]); /* Spawns thread or process */

    app->num_tabs++;           /* Increment total tab count */
    gui_switch_tab(app, idx);  /* Switch focus to newly opened tab */
}

/*
 * -------------------------------------------------------------------------------------
 * Function: gui_close_tab
 *
 * Purpose:
 *   Closes a specific tab identified by its index (0 .. num_tabs - 1).
 *   Safely:
 *     1. Shifts heap pointers in app->tabs and app->buffers arrays to preserve order.
 *     2. Invokes shell_tab_close() to cleanly join threads or signal child processes.
 *     3. Deallocates heap memory without dangling pointers or race conditions.
 *     4. Adjusts active_tab index or terminates application if all tabs are closed.
 *
 * Parameters:
 *   @app:     Pointer to GuiApp structure.
 *   @tab_idx: Zero-based index of the tab to close.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
void gui_close_tab(GuiApp *app, int tab_idx) {
    if (!app || tab_idx < 0 || tab_idx >= app->num_tabs) return;

    /* Hold references to the closing tab's objects */
    TabState *closing_tab = app->tabs[tab_idx];
    TermBuffer *closing_buf = app->buffers[tab_idx];

    /* Shift remaining tab pointers to the left */
    for (int i = tab_idx; i < app->num_tabs - 1; i++) {
        app->tabs[i] = app->tabs[i + 1];
        app->buffers[i] = app->buffers[i + 1];
    }
    app->tabs[app->num_tabs - 1] = NULL;
    app->buffers[app->num_tabs - 1] = NULL;
    app->num_tabs--; /* Decrement total tab count */

    /* Terminate worker and free allocated memory */
    if (closing_tab) {
        shell_tab_close(closing_tab); /* Join thread / wait on process */
        free(closing_tab);            /* Free heap TabState */
    }
    if (closing_buf) {
        buffer_free(closing_buf);     /* Free buffer lines */
        free(closing_buf);            /* Free heap TermBuffer */
    }

    /* If no tabs remain, terminate the GUI main loop */
    if (app->num_tabs == 0) {
        app->running = 0;
    } else {
        /* Adjust active tab index */
        if (tab_idx < app->active_tab) {
            app->active_tab--;
        } else if (app->active_tab >= app->num_tabs) {
            app->active_tab = app->num_tabs - 1;
        }
        gui_switch_tab(app, app->active_tab);
    }
}

/*
 * -------------------------------------------------------------------------------------
 * Function: gui_switch_tab
 *
 * Purpose:
 *   Changes the active (focused) tab. Updates the is_active flag on all tabs
 *   so background tabs know they are inactive, and triggers a full window redraw.
 *
 * Parameters:
 *   @app:     Pointer to GuiApp structure.
 *   @tab_idx: Zero-based index of tab to focus.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
void gui_switch_tab(GuiApp *app, int tab_idx) {
    if (!app || tab_idx < 0 || tab_idx >= app->num_tabs) return;

    for (int i = 0; i < app->num_tabs; i++) {
        if (app->tabs[i]) {
            app->tabs[i]->is_active = (i == tab_idx); /* Set 1 for active tab, 0 for inactive */
        }
    }
    app->active_tab = tab_idx;
    gui_redraw(app); /* Redraw screen to reflect newly active tab */
}

/*
 * -------------------------------------------------------------------------------------
 * Function: gui_redraw
 *
 * Purpose:
 *   Renders the entire application UI:
 *     1. Clears window background.
 *     2. Draws top Tab Bar:
 *        - Iterates over all open tabs.
 *        - Computes dynamic width using XTextWidth().
 *        - Draws active pill highlight or inactive background.
 *        - Draws "[ ID: <short_cwd> ", followed by red 'x' close button, followed by " ]".
 *        - Draws "[ + ]" new tab button.
 *     3. Renders the active tab's text buffer:
 *        - Acquires pthread mutex (tb->lock) for thread safety.
 *        - Factors in scroll offset (scrollback buffer).
 *        - Renders visible rows of text using XDrawString().
 *        - Draws block cursor at current cursor column.
 *        - Releases pthread mutex.
 *     4. Flushes drawing queue to X server via XFlush().
 *
 * Parameters:
 *   @app: Pointer to GuiApp structure.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
void gui_redraw(GuiApp *app) {
    if (!app || !app->display || app->num_tabs == 0) return;

    /* 1. Clear entire window background */
    XSetForeground(app->display, app->gc, app->col_bg);
    XFillRectangle(app->display, app->window, app->gc, 0, 0, app->win_width, app->win_height);

    /* 2. Draw Tab Bar background strip */
    XSetForeground(app->display, app->gc, app->col_tab_bg);
    XFillRectangle(app->display, app->window, app->gc, 0, 0, app->win_width, app->tab_bar_height);

    /* Draw separator line beneath the tab bar */
    XSetForeground(app->display, app->gc, app->col_border);
    XDrawLine(app->display, app->window, app->gc, 0, app->tab_bar_height, app->win_width, app->tab_bar_height);

    int tab_x = 8; /* Left horizontal starting coordinate for drawing tabs */
    for (int i = 0; i < app->num_tabs; i++) {
        if (!app->tabs[i]) continue;
        char short_cwd[MAX_PATH_LEN];
        shorten_path(app->tabs[i]->cwd, short_cwd, sizeof(short_cwd)); /* Replace /home/... with ~ */

        /* Format tab label prefix: "[ ID: <cwd>  " */
        char prefix[MAX_PATH_LEN + 64];
        snprintf(prefix, sizeof(prefix), "[ %d: %s  ", app->tabs[i]->id, short_cwd);
        const char *cross = "x";
        const char *suffix = " ]";

        /* Compute pixel widths of components using font metrics */
        int w_prefix = XTextWidth(app->font, prefix, strlen(prefix));
        int w_cross = XTextWidth(app->font, cross, strlen(cross));
        int w_suffix = XTextWidth(app->font, suffix, strlen(suffix));
        int tw = w_prefix + w_cross + w_suffix + 16; /* Total tab width with padding */

        /* Draw active tab pill highlight */
        if (i == app->active_tab) {
            XSetForeground(app->display, app->gc, app->col_tab_active);
            XFillRectangle(app->display, app->window, app->gc, tab_x, 2, tw, app->tab_bar_height - 2);

            XSetForeground(app->display, app->gc, app->col_tab_active_text);
        } else {
            XSetForeground(app->display, app->gc, app->col_tab_text);
        }

        /* Draw title prefix: "[ ID: <cwd>  " */
        XDrawString(app->display, app->window, app->gc,
                    tab_x + 8, app->font_ascent + 6,
                    prefix, strlen(prefix));

        /* Draw red cross sign 'x' for closing the tab */
        XSetForeground(app->display, app->gc, app->col_tab_close);
        XDrawString(app->display, app->window, app->gc,
                    tab_x + 8 + w_prefix, app->font_ascent + 6,
                    cross, strlen(cross));

        /* Draw closing bracket: " ]" */
        XSetForeground(app->display, app->gc,
                       (i == app->active_tab) ? app->col_tab_active_text : app->col_tab_text);
        XDrawString(app->display, app->window, app->gc,
                    tab_x + 8 + w_prefix + w_cross, app->font_ascent + 6,
                    suffix, strlen(suffix));

        tab_x += tw + 4; /* Advance x coordinate for next tab */
    }

    /* Draw "[ + ]" new tab button */
    const char *add_btn = "[ + ]";
    XSetForeground(app->display, app->gc, app->col_tab_text);
    XDrawString(app->display, app->window, app->gc, tab_x + 6, app->font_ascent + 6, add_btn, strlen(add_btn));

    /* 3. Render Active Tab Text Buffer */
    TermBuffer *tb = app->buffers[app->active_tab];
    if (!tb) return;
    /* Lock buffer mutex to prevent race conditions while worker thread writes output */
    pthread_mutex_lock(&tb->lock);

    int line_h = app->char_height;
    int area_top = app->tab_bar_height + 4;
    int avail_height = app->win_height - area_top;
    int visible_rows = avail_height / line_h; /* Maximum text rows that fit on screen */
    if (visible_rows <= 0) visible_rows = 1;

    /* Calculate visible line window factoring in scroll_offset */
    int total = tb->line_count;
    int end_idx = total - tb->scroll_offset;
    if (end_idx > total) end_idx = total;
    int start_idx = end_idx - visible_rows;
    if (start_idx < 0) start_idx = 0;

    XSetForeground(app->display, app->gc, app->col_fg);

    /* Render visible text lines */
    int row_draw = 0;
    for (int r = start_idx; r < end_idx; r++) {
        const char *line = tb->lines[r];
        if (line && strlen(line) > 0) {
            int y = area_top + row_draw * line_h + app->font_ascent;
            XDrawString(app->display, app->window, app->gc, 8, y, line, strlen(line));
        }
        row_draw++;
    }

    /* Draw block cursor at cursor position on current bottom line (if not scrolled up) */
    if (tb->scroll_offset == 0) {
        int cur_x = 8 + tb->cursor_col * app->char_width;
        int cur_y = area_top + (row_draw > 0 ? row_draw - 1 : 0) * line_h + 2;
        XSetForeground(app->display, app->gc, app->col_fg);
        XFillRectangle(app->display, app->window, app->gc, cur_x, cur_y, app->char_width, line_h - 2);
    }

    pthread_mutex_unlock(&tb->lock); /* Release buffer mutex lock */
    XFlush(app->display);            /* Flush queued drawing commands to X server */
}

/*
 * -------------------------------------------------------------------------------------
 * Function: handle_click (static helper)
 *
 * Purpose:
 *   Handles mouse click events (button press):
 *     - Clicks in Tab Bar:
 *       - Detects click on red 'x' close button or middle click -> closes tab.
 *       - Detects click on tab body -> switches to clicked tab.
 *       - Detects click on "[ + ]" button -> creates new tab.
 *     - Clicks in Terminal Body:
 *       - Mouse wheel scroll up (Button 4) -> scrolls buffer up.
 *       - Mouse wheel scroll down (Button 5) -> scrolls buffer down.
 *
 * Parameters:
 *   @app:     Pointer to GuiApp structure.
 *   @mouse_x: X coordinate of click in window pixels.
 *   @mouse_y: Y coordinate of click in window pixels.
 *   @button:  Mouse button number (1=Left, 2=Middle, 3=Right, 4=WheelUp, 5=WheelDown).
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static void handle_click(GuiApp *app, int mouse_x, int mouse_y, int button) {
    if (mouse_y <= app->tab_bar_height) {
        /* Click occurred inside the Tab Bar */
        int tab_x = 8;
        for (int i = 0; i < app->num_tabs; i++) {
            if (!app->tabs[i]) continue;
            char short_cwd[MAX_PATH_LEN];
            shorten_path(app->tabs[i]->cwd, short_cwd, sizeof(short_cwd));

            char prefix[MAX_PATH_LEN + 64];
            snprintf(prefix, sizeof(prefix), "[ %d: %s  ", app->tabs[i]->id, short_cwd);
            const char *cross = "x";
            const char *suffix = " ]";

            int w_prefix = XTextWidth(app->font, prefix, strlen(prefix));
            int w_cross = XTextWidth(app->font, cross, strlen(cross));
            int w_suffix = XTextWidth(app->font, suffix, strlen(suffix));
            int tw = w_prefix + w_cross + w_suffix + 16;

            /* Check if click was within this tab's horizontal bounds */
            if (mouse_x >= tab_x && mouse_x <= tab_x + tw) {
                /* Cross button hit region: starting just before 'x' to end of tab */
                int cross_x_start = tab_x + 8 + w_prefix - 4;

                /* Left-click on 'x' or Middle-click anywhere on tab closes the tab */
                if (mouse_x >= cross_x_start || button == 2) {
                    gui_close_tab(app, i);
                } else {
                    gui_switch_tab(app, i); /* Left-click on tab body switches active tab */
                }
                return;
            }
            tab_x += tw + 4;
        }

        /* Check if click was on the "[ + ]" add tab button */
        const char *add_btn = "[ + ]";
        int add_w = XTextWidth(app->font, add_btn, strlen(add_btn)) + 12;
        if (mouse_x >= tab_x && mouse_x <= tab_x + add_w) {
            gui_add_tab(app); /* Open a new tab */
            return;
        }
    } else {
        /* Click occurred inside the terminal buffer area: mouse wheel scrollback */
        if (button == 4) {
            /* Button 4: Scroll wheel UP */
            buffer_scroll_up(app->buffers[app->active_tab], 3);
            gui_redraw(app);
        } else if (button == 5) {
            /* Button 5: Scroll wheel DOWN */
            buffer_scroll_down(app->buffers[app->active_tab], 3);
            gui_redraw(app);
        }
    }
}

/*
 * -------------------------------------------------------------------------------------
 * Function: copy_selection (static helper)
 *
 * Purpose:
 *   Copies the active line from the terminal buffer into the application's clipboard
 *   buffer and asserts ownership of X11 CLIPBOARD and PRIMARY selection atoms.
 *
 * Parameters:
 *   @app: Pointer to GuiApp structure.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static void copy_selection(GuiApp *app) {
    TermBuffer *tb = app->buffers[app->active_tab];
    if (!tb) return;
    pthread_mutex_lock(&tb->lock);
    int cur_line = tb->line_count - 1;
    if (cur_line >= 0 && tb->lines[cur_line]) {
        strncpy(app->clipboard_data, tb->lines[cur_line], sizeof(app->clipboard_data) - 1);
        app->clipboard_data[sizeof(app->clipboard_data) - 1] = '\0';
    }
    pthread_mutex_unlock(&tb->lock);

    /* Assert ownership of X11 selections */
    XSetSelectionOwner(app->display, app->clipboard_atom, app->window, CurrentTime);
    XSetSelectionOwner(app->display, XA_PRIMARY, app->window, CurrentTime);
}

/*
 * -------------------------------------------------------------------------------------
 * Function: paste_selection (static helper)
 *
 * Purpose:
 *   Requests the current system clipboard text from whatever application currently
 *   owns the CLIPBOARD selection via XConvertSelection(). The server responds with
 *   a SelectionNotify event handled in gui_run().
 *
 * Parameters:
 *   @app: Pointer to GuiApp structure.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static void paste_selection(GuiApp *app) {
    XConvertSelection(app->display, app->clipboard_atom, XA_STRING,
                      app->clipboard_atom, app->window, CurrentTime);
}

/*
 * -------------------------------------------------------------------------------------
 * Function: send_string_to_active_tab (static helper)
 *
 * Purpose:
 *   Takes a string (e.g. from clipboard paste) and transmits it character-by-character
 *   as TermKeyEvents into the active tab's IPC pipe.
 *
 * Parameters:
 *   @app: Pointer to GuiApp structure.
 *   @str: String text to paste.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static void send_string_to_active_tab(GuiApp *app, const char *str) {
    if (!str || app->num_tabs == 0 || !app->tabs[app->active_tab]) return;
    int len = strlen(str);
    for (int i = 0; i < len; i++) {
        TermKeyEvent ev;
        if (str[i] == '\n') {
            ev.code = KEY_CODE_ENTER;
            ev.ch = '\n';
        } else {
            ev.code = KEY_CODE_CHAR;
            ev.ch = str[i];
        }
        shell_tab_send_key(app->tabs[app->active_tab], &ev);
    }
}

/*
 * -------------------------------------------------------------------------------------
 * Function: handle_keypress (static helper)
 *
 * Purpose:
 *   Translates raw XKeyEvent events from the X server into Terminator KeyEvents:
 *     1. Checks GUI-level hotkeys (Ctrl+Shift+T = new tab, Ctrl+Shift+W = close tab,
 *        Ctrl+Tab / Ctrl+PageDown = next tab, Ctrl+PageUp = prev tab).
 *     2. Checks buffer scrollback (Shift+PageUp / Shift+PageDown).
 *     3. Checks custom ~/.terminatorrc mapped actions (ACT_COPY, ACT_PASTE, etc.).
 *     4. Dispatches POSIX signals (Ctrl+C = SIGINT, Ctrl+Z = SIGTSTP).
 *     5. Forwards standard line-editing keys (Enter, Backspace, Arrows, Tab, Ctrl+R, etc.)
 *        to the active tab worker via shell_tab_send_key().
 *
 * Parameters:
 *   @app: Pointer to GuiApp structure.
 *   @e:   Pointer to XKeyEvent structure received from XNextEvent().
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
static void handle_keypress(GuiApp *app, XKeyEvent *e) {
    if (app->num_tabs == 0) return;

    KeySym ks;
    char char_buf[32];
    /* XLookupString(): Translates key event to KeySym and ASCII character string */
    int nchars = XLookupString(e, char_buf, sizeof(char_buf) - 1, &ks, NULL);
    char_buf[nchars] = '\0';

    unsigned int state = e->state;
    int ctrl = (state & ControlMask);  /* 1 if Ctrl key is pressed */
    int shift = (state & ShiftMask);   /* 1 if Shift key is pressed */

    /* 1. GUI level Tab navigation */
    if (ctrl && shift && (ks == XK_T || ks == XK_t)) {
        gui_add_tab(app); /* Ctrl+Shift+T: Add new tab */
        return;
    }
    if (ctrl && shift && (ks == XK_W || ks == XK_w)) {
        gui_close_tab(app, app->active_tab); /* Ctrl+Shift+W: Close current tab */
        return;
    }
    if (ctrl && (ks == XK_Page_Down || ks == XK_Tab)) {
        gui_switch_tab(app, (app->active_tab + 1) % app->num_tabs); /* Next tab */
        return;
    }
    if (ctrl && (ks == XK_Page_Up)) {
        gui_switch_tab(app, (app->active_tab - 1 + app->num_tabs) % app->num_tabs); /* Prev tab */
        return;
    }

    /* 2. Scrollback buffer navigation */
    if (shift && ks == XK_Page_Up) {
        if (app->buffers[app->active_tab]) buffer_scroll_up(app->buffers[app->active_tab], 10);
        gui_redraw(app);
        return;
    }
    if (shift && ks == XK_Page_Down) {
        if (app->buffers[app->active_tab]) buffer_scroll_down(app->buffers[app->active_tab], 10);
        gui_redraw(app);
        return;
    }

    /* 3. Check ~/.terminatorrc configured action */
    ActionType act = config_match_action(&app->config, ks, state);
    if (act == ACT_COPY) {
        copy_selection(app); /* Copy text to clipboard */
        return;
    }
    if (act == ACT_PASTE) {
        paste_selection(app); /* Paste text from clipboard */
        return;
    }

    /* 4. POSIX Signals: Ctrl+C (SIGINT) and Ctrl+Z (SIGTSTP) */
    if (ctrl && (ks == XK_c || ks == XK_C) && !shift) {
        if (app->tabs[app->active_tab]) shell_tab_send_signal(app->tabs[app->active_tab], SIGINT);
        return;
    }
    if (ctrl && (ks == XK_z || ks == XK_Z) && !shift) {
        if (app->tabs[app->active_tab]) shell_tab_send_signal(app->tabs[app->active_tab], SIGTSTP);
        return;
    }

    /* 5. Map ActionType or KeySym to TermKeyEvent */
    TermKeyEvent ev;
    ev.code = KEY_CODE_NONE;
    ev.ch = '\0';

    switch (act) {
        case ACT_MOVE_LEFT:    ev.code = KEY_CODE_LEFT; break;
        case ACT_MOVE_RIGHT:   ev.code = KEY_CODE_RIGHT; break;
        case ACT_MOVE_START:   ev.code = KEY_CODE_CTRL_A; break;
        case ACT_MOVE_END:     ev.code = KEY_CODE_CTRL_E; break;
        case ACT_HIST_PREV:    ev.code = KEY_CODE_UP; break;
        case ACT_HIST_NEXT:    ev.code = KEY_CODE_DOWN; break;
        case ACT_DEL_START:    ev.code = KEY_CODE_CTRL_U; break;
        case ACT_DEL_END:      ev.code = KEY_CODE_CTRL_K; break;
        case ACT_DEL_WORD:     ev.code = KEY_CODE_CTRL_W; break;
        case ACT_CLEAR_SCREEN: ev.code = KEY_CODE_CTRL_L; break;
        case ACT_EXIT_SHELL:   ev.code = KEY_CODE_CTRL_D; break;
        default: break;
    }

    if (ev.code == KEY_CODE_NONE) {
        if (ctrl && (ks == XK_r || ks == XK_R)) {
            ev.code = KEY_CODE_CTRL_R; /* Ctrl+R: Reverse history search */
        } else if (ks == XK_Return || ks == XK_KP_Enter) {
            ev.code = KEY_CODE_ENTER;
            ev.ch = '\n';
        } else if (ks == XK_BackSpace) {
            ev.code = KEY_CODE_BACKSPACE;
        } else if (ks == XK_Delete) {
            ev.code = KEY_CODE_DELETE;
        } else if (ks == XK_Tab) {
            ev.code = KEY_CODE_TAB;
        } else if (ks == XK_Left) {
            ev.code = KEY_CODE_LEFT;
        } else if (ks == XK_Right) {
            ev.code = KEY_CODE_RIGHT;
        } else if (ks == XK_Up) {
            ev.code = KEY_CODE_UP;
        } else if (ks == XK_Down) {
            ev.code = KEY_CODE_DOWN;
        } else if (nchars > 0) {
            ev.code = KEY_CODE_CHAR;
            ev.ch = char_buf[0];
        }
    }

    /* Send key event over IPC pipe to active tab's worker thread/process */
    if (ev.code != KEY_CODE_NONE) {
        if (app->tabs[app->active_tab]) shell_tab_send_key(app->tabs[app->active_tab], &ev);
    }
}

/*
 * -------------------------------------------------------------------------------------
 * Function: gui_run
 *
 * Purpose:
 *   The primary runtime event loop of the Terminator GUI:
 *     1. Non-blockingly drains all pending X11 window events (Expose, KeyPress, ButtonPress,
 *        ConfigureNotify resize, SelectionRequest, WM_DELETE_WINDOW).
 *     2. Sets up a file descriptor read set (fd_set) containing:
 *        - The X11 connection socket descriptor (ConnectionNumber(display)).
 *        - All open tab worker output pipes (from_tab_fd).
 *     3. Calls select() with a 25ms timeout to avoid busy-waiting and reduce CPU usage to 0%.
 *     4. When tab output arrives, reads text from pipe:
 *        - Detects Operating System Command (OSC) sequence \033]0;title\007 to dynamically
 *          update tab titles when 'cd' is called.
 *        - Appends output to the tab's TermBuffer.
 *        - Detects EOF (read() returns 0) when a shell exits and automatically closes the tab.
 *     5. Redraws UI whenever output arrives or window needs refresh.
 *
 * Parameters:
 *   @app: Pointer to GuiApp structure.
 *
 * Returns:
 *   void.
 * -------------------------------------------------------------------------------------
 */
void gui_run(GuiApp *app) {
    if (!app || !app->display) return;

    /* ConnectionNumber(): Returns underlying UNIX/TCP socket file descriptor for X server */
    int x11_fd = ConnectionNumber(app->display);

    while (app->running) {
        /* 1. Process all pending X11 events queued on client side */
        while (XPending(app->display)) {
            XEvent ev;
            XNextEvent(app->display, &ev); /* Dequeue next X event */

            switch (ev.type) {
                case Expose:
                    /* Window damaged or uncovered: redraw when final expose event arrives */
                    if (ev.xexpose.count == 0) {
                        gui_redraw(app);
                    }
                    break;
                case ConfigureNotify:
                    /* Window resized by user or window manager */
                    app->win_width = ev.xconfigure.width;
                    app->win_height = ev.xconfigure.height;
                    gui_redraw(app);
                    break;
                case ButtonPress:
                    /* Mouse button clicked */
                    handle_click(app, ev.xbutton.x, ev.xbutton.y, ev.xbutton.button);
                    break;
                case KeyPress:
                    /* Keyboard key pressed */
                    handle_keypress(app, &ev.xkey);
                    break;
                case SelectionRequest: {
                    /* Another application requested our clipboard text */
                    XSelectionRequestEvent *req = &ev.xselectionrequest;
                    XEvent resp;
                    memset(&resp, 0, sizeof(resp));
                    resp.xselection.type = SelectionNotify;
                    resp.xselection.display = req->display;
                    resp.xselection.requestor = req->requestor;
                    resp.xselection.selection = req->selection;
                    resp.xselection.target = req->target;
                    resp.xselection.time = req->time;
                    resp.xselection.property = req->property;

                    if (req->target == XA_STRING || req->target == app->utf8_string_atom) {
                        XChangeProperty(app->display, req->requestor, req->property,
                                        req->target, 8, PropModeReplace,
                                        (unsigned char *)app->clipboard_data,
                                        strlen(app->clipboard_data));
                    } else if (req->target == app->targets_atom) {
                        Atom targets[] = { XA_STRING, app->utf8_string_atom };
                        XChangeProperty(app->display, req->requestor, req->property,
                                        XA_ATOM, 32, PropModeReplace,
                                        (unsigned char *)targets, 2);
                    } else {
                        resp.xselection.property = None;
                    }
                    XSendEvent(app->display, req->requestor, False, 0, &resp);
                    XFlush(app->display);
                    break;
                }
                case SelectionNotify: {
                    /* Received clipboard text requested by paste_selection() */
                    XSelectionEvent *sel = &ev.xselection;
                    if (sel->property != None) {
                        Atom type;
                        int format;
                        unsigned long nitems, bytes_after;
                        unsigned char *prop_data = NULL;

                        if (XGetWindowProperty(app->display, sel->requestor, sel->property,
                                               0, 65536, True, AnyPropertyType,
                                               &type, &format, &nitems, &bytes_after,
                                               &prop_data) == Success && prop_data) {
                            send_string_to_active_tab(app, (char *)prop_data);
                            XFree(prop_data);
                        }
                    }
                    break;
                }
                case ClientMessage:
                    /* User clicked the window frame close button ('X') */
                    if ((Atom)ev.xclient.data.l[0] == app->wm_delete_window) {
                        app->running = 0; /* Signal loop to terminate */
                    }
                    break;
                default:
                    break;
            }
        }

        if (!app->running) break;

        /* 2. Prepare file descriptor set for select() multiplexing */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(x11_fd, &rfds); /* Monitor X11 socket */
        int max_fd = x11_fd;

        /* Add output pipes of all open tabs */
        for (int i = 0; i < app->num_tabs; i++) {
            if (app->tabs[i] && app->tabs[i]->from_tab_fd >= 0) {
                FD_SET(app->tabs[i]->from_tab_fd, &rfds);
                if (app->tabs[i]->from_tab_fd > max_fd) {
                    max_fd = app->tabs[i]->from_tab_fd;
                }
            }
        }

        /* 3. select(): Wait until data is ready on X11 socket or tab pipes, with 25ms timeout */
        struct timeval tv = { .tv_sec = 0, .tv_usec = 25000 }; /* 25 milliseconds */
        int sel_ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);

        if (sel_ret < 0) {
            if (errno == EINTR) continue; /* Interrupted by signal: retry */
            break;                        /* Fatal select error */
        }

        /* 4. Read output from any tab pipe that has data ready */
        int needs_redraw = 0;
        for (int i = 0; i < app->num_tabs; i++) {
            if (app->tabs[i] && app->tabs[i]->from_tab_fd >= 0 && FD_ISSET(app->tabs[i]->from_tab_fd, &rfds)) {
                char buf[16384];
                ssize_t n = read(app->tabs[i]->from_tab_fd, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';

                    /* Detect OSC sequence \033]0;title\007 for dynamic tab title updates */
                    char *osc = strstr(buf, "\033]0;");
                    if (osc) {
                        char *osc_end = strchr(osc, '\007');
                        if (osc_end) {
                            *osc_end = '\0';
                            /* Update tab's current working directory */
                            snprintf(app->tabs[i]->cwd, sizeof(app->tabs[i]->cwd), "%s", osc + 4);
                            /* Remove OSC sequence from user-visible buffer text */
                            memmove(osc, osc_end + 1, strlen(osc_end + 1) + 1);
                            n = strlen(buf);
                            needs_redraw = 1;
                        }
                    }

                    if (n > 0) {
                        if (app->buffers[i]) {
                            buffer_append_text(app->buffers[i], buf, n); /* Append output to buffer */
                        }
                        if (i == app->active_tab) {
                            needs_redraw = 1;
                        }
                    }
                } else if (n == 0) {
                    /* read() returned 0: Pipe EOF detected (shell worker has exited) */
                    gui_close_tab(app, i);
                    i--; /* Decrement index to account for shifted array */
                    needs_redraw = 1;
                }
            }
        }

        /* 5. If buffer changed, refresh GUI */
        if (needs_redraw) {
            gui_redraw(app);
        }
    }
}
