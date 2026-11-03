/*
 * =====================================================================================
 * File: include/gui.h
 * Project: Terminator - Custom Tabbed Terminal Emulator & Shell
 *
 * Educational Purpose & Viva Guide:
 * This header defines the Graphical User Interface (GUI) state structure (GuiApp)
 * and declares the public GUI lifecycle and manipulation functions.
 * It integrates Xlib (X11) graphical primitives with Terminator's tabbed buffer
 * and shell worker architecture.
 * =====================================================================================
 */

#ifndef GUI_H
#define GUI_H

#include "terminator.h"  /* Global limits, types, and standard library headers */
#include "buffer.h"      /* TermBuffer screen buffer structure */
#include "config.h"      /* User configuration and hotkey structures */
#include "shell.h"       /* TabState and shell worker execution */
#include <X11/Xlib.h>    /* Core X11 client library (Display, Window, GC, XEvent) */
#include <X11/Xutil.h>   /* X11 utility functions (XLookupString, SizeHints) */
#include <X11/keysym.h>  /* Standard X11 KeySym constants (XK_Return, XK_Tab, etc.) */
#include <X11/Xatom.h>   /* Standard X11 Atoms (XA_STRING, XA_PRIMARY, etc.) */

/*
 * Structure: GuiApp
 * Purpose:
 *   Encapsulates the complete state of the Terminator X11 GUI application:
 *   - X11 connection objects: Display, Screen, Window, Graphics Context (GC), Font.
 *   - Font cell metrics: char_width, char_height, font_ascent for grid placement.
 *   - Palette color pixels: background, foreground, tab highlights, red close cross.
 *   - Window geometry: width, height, tab bar height.
 *   - Tab management: heap pointer arrays tabs[MAX_TABS] and buffers[MAX_TABS].
 *   - X11 ICCCM clipboard Atoms: CLIPBOARD, TARGETS, UTF8_STRING.
 *   - Main loop control flag: running.
 */
typedef struct {
    Display *display;             /* Pointer to active connection with the X11 server */
    int screen;                   /* Screen number (usually default screen 0) */
    Window window;                /* X11 Window ID representing the application window */
    GC gc;                        /* Graphics Context holding drawing state (color, font) */
    XFontStruct *font;            /* Loaded monospace font metrics and glyph information */
    int char_width;               /* Fixed character cell width in pixels */
    int char_height;              /* Fixed character cell height in pixels */
    int font_ascent;              /* Baseline distance from the top of the character cell */

    /* Pre-allocated X11 color pixel values */
    unsigned long col_fg;             /* Main terminal text color (default: #d4d4d4) */
    unsigned long col_bg;             /* Terminal background color (default: #1e1e1e) */
    unsigned long col_tab_bg;         /* Tab bar background strip color (#2d2d2d) */
    unsigned long col_tab_active;     /* Active tab pill highlight color (#3e3e42) */
    unsigned long col_tab_text;       /* Inactive tab label text color (#969696) */
    unsigned long col_tab_active_text;/* Active tab label text color (#ffffff) */
    unsigned long col_tab_close;      /* Tab 'x' close button color (coral red #e06c75) */
    unsigned long col_border;         /* Separator border line color (#3f3f46) */

    int win_width;                /* Current window width in pixels */
    int win_height;               /* Current window height in pixels */
    int tab_bar_height;           /* Height of top tab navigation bar in pixels */

    /* Dynamic tab array: heap pointers prevent stack overflow and race conditions */
    TabState *tabs[MAX_TABS];         /* Pointers to shell worker states for open tabs */
    TermBuffer *buffers[MAX_TABS];    /* Pointers to scrollback screen buffers for open tabs */
    int num_tabs;                     /* Current number of active open tabs */
    int active_tab;                   /* Index of currently focused tab (0 .. num_tabs-1) */

    ExecutionMode mode;           /* Tab concurrency mode: MODE_THREAD or MODE_PROCESS */
    AppConfig config;             /* Parsed configuration from ~/.terminatorrc */

    /* X11 Atoms for Window Manager and Clipboard protocols */
    Atom wm_delete_window;        /* Protocol atom for window frame close button ('X') */
    Atom clipboard_atom;          /* Atom for "CLIPBOARD" selection */
    Atom targets_atom;            /* Atom for "TARGETS" format negotiation */
    Atom utf8_string_atom;        /* Atom for "UTF8_STRING" encoding */

    char clipboard_data[65536];   /* Internal clipboard buffer storing copied text */
    int running;                  /* Main event loop active flag (1=running, 0=exit) */
} GuiApp;

/*
 * GUI Lifecycle Functions:
 */

/* Initializes X11 display, fonts, window, colors, and creates the first tab */
int gui_init(GuiApp *app, ExecutionMode mode);

/* Releases X11 resources, closes and frees all tabs and memory */
void gui_cleanup(GuiApp *app);

/* Runs the non-blocking select() event loop multiplexing X11 events and tab pipes */
void gui_run(GuiApp *app);

/*
 * Tab Manipulation Functions:
 */

/* Spawns a new terminal tab with the lowest available recycled ID */
void gui_add_tab(GuiApp *app);

/* Closes a specific tab, joins its thread / reaps process, and shifts pointers */
void gui_close_tab(GuiApp *app, int tab_idx);

/* Switches the active focused tab and triggers a redraw */
void gui_switch_tab(GuiApp *app, int tab_idx);

/* Completely redraws the window: clears bg, draws tab bar with close 'x', renders text */
void gui_redraw(GuiApp *app);

#endif /* GUI_H */
