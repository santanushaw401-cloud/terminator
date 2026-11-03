/*
 * ==============================================================================
 * Project:      Terminator - A Custom Linux Terminal & Shell
 * File:         src/main.c
 * Purpose:      Main entry point of the entire application.
 * Description:  Parses command-line arguments (e.g., thread mode vs process mode),
 *               ignores dangerous terminating signals (SIGPIPE), initializes the
 *               cross-tab shared history subsystem, allocates and starts the X11 GUI,
 *               runs the main event dispatcher, and performs clean memory teardown on exit.
 *
 * Viva / Examination Notes:
 * 1. Why do we need ExecutionMode (Thread vs Process)?
 *    - In "Thread Mode" (default), each terminal tab runs inside a POSIX thread (pthread).
 *      They share the same process memory space and are very lightweight.
 *    - In "Process Mode" (useProcess=True), each tab forks into an independent UNIX process.
 *      They have completely isolated address spaces and communicate via pipes & shared memory.
 * 2. Why ignore SIGPIPE?
 *    - When a tab closes its pipe and another component tries to write to it, Linux by
 *      default sends SIGPIPE, which immediately crashes the entire application.
 *      Setting signal(SIGPIPE, SIG_IGN) ensures the write() call simply returns -1 with EPIPE
 *      instead of killing Terminator.
 * ==============================================================================
 */

#include "terminator.h" /* Global constants, version macro, and standard library headers */
#include "gui.h"        /* Declarations for X11 GUI window, tabs, and the event loop */
#include "history.h"    /* Declarations for persistent shared history & Ctrl+R search */

/*
 * Function:    print_usage
 * Purpose:     Displays helpful instructions explaining how to run Terminator from CLI.
 * Parameters:  prog - The executable binary name (argv[0], e.g., "./terminator").
 * Returns:     void (no return value).
 * Explanation: Called whenever the user provides -h or --help flags.
 */
static void print_usage(const char *prog) {
    /* Print project title banner */
    printf("Terminator: A Custom Terminal\n");
    /* Print syntax examples for launching in thread mode or process mode */
    printf("Usage:\n");
    printf("  %s                      # Thread mode (default)\n", prog);
    printf("  %s useProcess=True      # Process mode\n", prog);
    /* Print supported command line flags */
    printf("Options:\n");
    printf("  -h, --help              # Show this help message\n");
    printf("  -v, --version           # Show version\n");
}

/*
 * Function:    main
 * Purpose:     The primary entry point executed by the Linux OS kernel when running Terminator.
 * Parameters:  argc - Argument count (number of command line strings provided).
 *              argv - Array of null-terminated argument strings.
 * Returns:     int  - Exit status code: 0 on successful termination, 1 on initialization error.
 */
int main(int argc, char **argv) {
    /* Default execution mode: Thread Mode (each tab runs as a pthread inside one process) */
    ExecutionMode mode = MODE_THREAD;

    /* Loop through command-line arguments starting from index 1 (argv[0] is program name) */
    for (int i = 1; i < argc; i++) {
        /* Check if the user requested Process Mode instead of Thread Mode */
        if (strcmp(argv[i], "useProcess=True") == 0) {
            /* Switch mode flag to MODE_PROCESS (each tab will be an independent forked process) */
            mode = MODE_PROCESS;
        } 
        /* Check for help flags (-h or --help) */
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            /* Print usage instructions and exit successfully */
            print_usage(argv[0]);
            return 0; /* Return exit code 0 indicating clean exit */
        } 
        /* Check for version flags (-v or --version) */
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            /* Print the current version defined in include/terminator.h */
            printf("Terminator version %s\n", TERMINATOR_VERSION);
            return 0; /* Return exit code 0 indicating clean exit */
        }
    }

    /*
     * Signal handling: Ignore SIGPIPE.
     * Significance: In UNIX, if a process writes to a pipe whose reading end has closed,
     * the kernel raises a SIGPIPE signal whose default action terminates the process.
     * By setting SIGPIPE to SIG_IGN (ignore), write() safely fails with errno = EPIPE
     * without crashing the entire terminal emulator.
     */
    signal(SIGPIPE, SIG_IGN);

    /*
     * Initialize the Shared History Subsystem:
     * - In Thread Mode: Allocates a heap structure protected by a POSIX thread semaphore.
     * - In Process Mode: Creates POSIX Shared Memory (mmap with MAP_SHARED | MAP_ANONYMOUS)
     *   and a process-shared semaphore so all child tab processes can share command history.
     */
    history_init(mode);

    /*
     * Allocate memory on the heap for the GuiApp structure:
     * calloc() allocates memory and zeros out every byte, preventing uninitialized garbage.
     */
    GuiApp *app = (GuiApp *)calloc(1, sizeof(GuiApp));
    if (!app) {
        /* If memory allocation failed, clean up history before exiting */
        history_cleanup();
        return 1; /* Return non-zero status indicating failure */
    }

    /*
     * Initialize X11 GUI, create display connection, font metrics, and create Tab 1:
     * gui_init() returns 0 on success, or -1 if the X server cannot be contacted.
     */
    if (gui_init(app, mode) != 0) {
        /* Failed to open X11 display (e.g. DISPLAY environment variable not set) */
        free(app);            /* Free the allocated GuiApp structure */
        history_cleanup();    /* Clean up shared history resources */
        return 1;             /* Return 1 indicating failure */
    }

    /*
     * Enter the non-blocking main event loop:
     * Uses select() to listen simultaneously for X11 events (keystrokes, mouse clicks)
     * and output data arriving from all background tab shell pipes.
     * This loop runs until the user closes all tabs or clicks the window close button.
     */
    gui_run(app);

    /*
     * Graceful Application Teardown:
     * Clean up all X11 resources, destroy windows, join/kill remaining tab workers,
     * and close all open UNIX pipes.
     */
    gui_cleanup(app);

    /* Free the master GuiApp memory */
    free(app);

    /* Unmap shared memory and destroy history semaphore */
    history_cleanup();

    /* Return 0 indicating clean and successful application termination */
    return 0;
}
