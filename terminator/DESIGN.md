# Terminator: Design Document

**Computing Lab (CS69201) · Term Project**  
*Terminator* is a multi-tab terminal emulator and shell implemented in C for Linux using the X11 Xlib library. This document details the software architecture, design decisions, data structures, and operating system mechanisms.

---

## 1. System Architecture Overview

Terminator separates graphical presentation from shell execution through a clean IPC interface:
```
 +-------------------------------------------------------------+
 |                   GUI Parent Process (X11)                  |
 |  - Window creation, Xlib event polling (select loop)        |
 |  - Multi-tab management, scrolling text buffers             |
 |  - Clipboard integration (PRIMARY & CLIPBOARD atoms)        |
 +-------------------------------------------------------------+
            |                             |
      pipe / IPC                    pipe / IPC
            v                             v
 +-----------------------+     +-----------------------+
 |     Tab Shell 1       |     |     Tab Shell 2       |
 | (Thread or Fork Proc) |     | (Thread or Fork Proc) |
 |  - Per-tab CWD        |     |  - Per-tab CWD        |
 |  - Line editing       |     |  - Line editing       |
 |  - Job control        |     |  - Job control        |
 |  - Command parser     |     |  - Command parser     |
 +-----------------------+     +-----------------------+
            \                             /
             v                           v
  +-------------------------------------------------+
  |      Shared History Subsystem (POSIX Shm / Sem) |
  |  - 10,000 commands persistent ring buffer       |
  |  - Ctrl+R exact match & LCS search              |
  |  - ~/.terminator_history file backing           |
  +-------------------------------------------------+
```

---

## 2. X11 GUI and Event Loop

### 2.1 Window and Rendering Mechanism
The graphical interface is constructed directly using core Xlib:
- **Display Connection**: Initiated via `XOpenDisplay(NULL)`. The primary window is created via `XCreateSimpleWindow()` and mapped with `XMapWindow()`.
- **Typography & Font Metrics**: Loads monospace fonts (attempting `fixed`, `9x15`, and ISO-8859 font aliases) through `XLoadQueryFont()`. Fixed cell width (`char_width = max_bounds.width`) and height (`char_height = ascent + descent`) ensure consistent terminal grid alignment.
- **Tab Header Bar**: Rendered along the top border (`[ 1: ~/Downloads ] [ 2: /tmp ] [ + ]`). Mouse clicks inside tab headers calculate cell boundaries using `XTextWidth()` to dynamically switch active tabs or instantiate new tabs (`[ + ]`).
- **Text Scrollback Buffer**: Each tab owns a `TermBuffer` maintaining up to 2,000 lines. The screen renders visible lines offset by `scroll_offset`. Scrolling is supported via mouse wheel (buttons 4 and 5) or `Shift+PageUp` / `Shift+PageDown`.

### 2.2 Non-Blocking Event Loop (Zero Busy-Waiting)
Terminals must remain responsive to both windowing system events and background asynchronous process output without burning CPU cycles in tight polling loops.
Terminator implements a unified `select()` dispatcher:
1. `ConnectionNumber(display)` returns the underlying socket file descriptor for X server communications.
2. The read ends of all active tab output pipes (`tab->from_tab_fd`) are registered into an `fd_set`.
3. `XPending(display)` flushes already-buffered Xlib client events.
4. `select(max_fd + 1, &rfds, NULL, NULL, &tv)` puts the process to sleep until either:
   - A key or mouse event arrives from the X server,
   - Output bytes arrive from a running tab shell or child process,
   - Or a brief 25ms timer expires for cursor blinking and job status checks.

---

## 3. Execution Modes: Thread Mode vs Process Mode

Terminator supports two distinct execution architectures selected via command-line flags:
`./terminator` (Thread mode, default) vs `./terminator useProcess=True` (Process mode).

### 3.1 Thread Mode
- **Process Layout**: One unified process. The GUI operates on the main thread; each tab shell is spawned as a POSIX thread via `pthread_create()`.
- **Shared Memory**: Tabs operate in the same address space. Shared state (command history) is guarded by POSIX semaphores / mutexes.
- **Per-Tab Working Directory Isolation**:
  *Problem*: In Linux, `chdir()` alters the current working directory for the entire process, which would cause `cd` in one tab to alter all other tabs.
  *Solution*: Terminator tracks an isolated `cwd` string per `TabState`. When built-in commands or autocompletions run, they reference `tab->cwd`. When external commands are forked, the forked child invokes `chdir(tab->cwd)` right before `execvp()`. Consequently, directory state remains completely decoupled between tab threads.

### 3.2 Process Mode
- **Process Layout**: The GUI operates in the parent process. Each new tab is instantiated via `fork()`.
- **Inter-Process Communication**: Communication between the parent GUI and the tab process takes place across unidirectional pipe pairs (`to_tab_fd` and `from_tab_fd`).
- **Failure Isolation**: A crash or abnormal termination of a tab child process closes its pipe, signaling EOF to the GUI. The parent catches this and removes the tab cleanly without affecting sibling tabs.
- **Shared History across Processes**:
  The history ring buffer resides in a POSIX anonymous shared memory mapping (`mmap(..., MAP_SHARED | MAP_ANONYMOUS, ...)`). Synchronization uses a process-shared POSIX semaphore:
  ```c
  sem_init(&shm->sem, 1, 1); /* pshared = 1 */
  ```
  Both the parent and all forked tab processes inherit this memory mapping and synchronize through `sem_wait()` / `sem_post()`.

---

## 4. Process and Job Management

### 4.1 Process Groups & Foreground Execution
Whenever an external pipeline executes:
- The lead child process creates a distinct process group: `setpgid(0, 0)`.
- All subsequent child processes in the pipeline join this process group: `setpgid(0, leader_pid)`.
- The shell records `tab->jobs.fg_pgid = leader_pid` and blocks on `waitpid(-leader_pid, &status, WUNTRACED)`.

### 4.2 Background Execution (`cmd &`)
When a command string terminates with `&`:
- The shell does not block with `waitpid()`.
- It allocates a job descriptor in `tab->jobs`, records the process group ID, marks the state as `JOB_RUNNING`, and outputs `[<job_id>] <pgid>`.

### 4.3 Job Suspension (`Ctrl+Z`) and Management Builtins
- **Suspending**: When `Ctrl+Z` arrives, the GUI sends `SIGTSTP` to the active tab's foreground process group.
- Upon detecting `WIFSTOPPED(status)`, the shell saves the command in `tab->jobs` with state `JOB_STOPPED` and prints `[<job_id>]+ Stopped <cmd>`.
- **Built-in `jobs`**: Formats all currently tracked jobs with job ID, status (`Running` / `Stopped`), and current job indicator (`+` / `-`).
- **Built-in `bg %id`**: Sends `kill(-pgid, SIGCONT)` to resume the process in the background. State transitions to `JOB_RUNNING`.
- **Built-in `fg %id`**: Assigns the job's PGID to `tab->jobs.fg_pgid`, sends `SIGCONT`, removes the job entry from the list, and blocks in `waitpid()` until completion or suspension.

---

## 5. IPC Architecture (Pipes, Redirection, dup2)

### 5.1 Pipeline Chaining
For an arbitrary pipeline of $N$ commands (`cmd1 | cmd2 | ... | cmdN`), Terminator creates $N - 1$ pipes:
- Child $0$: Output directed to `pipe[0][1]`.
- Child $i$ ($0 < i < N - 1$): Input from `pipe[i - 1][0]`, output to `pipe[i][1]`.
- Child $N - 1$: Input from `pipe[N - 2][0]`, output redirected to the GUI terminal pipe `out_fd`.
- File descriptor hygiene: All unused read and write ends are closed immediately in both children and parent to prevent hangs waiting for EOF.

### 5.2 I/O Redirection
- **Input Redirection (`<`)**: Prior to child execution, Terminator verifies input file accessibility (`access(path, R_OK)`). If missing, an error is reported immediately and no child process is launched. Inside the child, `open(infile, O_RDONLY)` followed by `dup2(fd, STDIN_FILENO)` binds the stream.
- **Output Redirection (`>` and `>>`)**: `open(outfile, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644)` followed by `dup2(fd, STDOUT_FILENO)` safely redirects stdout. Background jobs opening independent descriptors do not interfere with each other.

### 5.3 multiWatch Parallel Polling
`multiWatch ["cmd1", "cmd2", ...]` spawns dedicated child processes for every command simultaneously:
- Each command child has its own pipe.
- The parent monitors all active pipe read ends via `select()`.
- When child output arrives, it is captured into an internal buffer. Upon EOF, the parent reaps the child with `waitpid()` and renders the output block with high-precision timestamp headers:
  ```
  "cmd1", <timestamp>:
  ----------------------------------------------------
  <output>
  ----------------------------------------------------
  ```
- Commands auto-repeat every 2 seconds after finishing.
- Pressing `Ctrl+C` sends `kill(pid, SIGINT)` to all running children, flushes output, prints `^C`, and returns to the shell.

---

## 6. Data Structures

### 6.1 Shared History Buffer
```c
typedef struct {
    sem_t sem;                                          /* Process-shared semaphore */
    int count;                                          /* Number of active entries */
    char commands[MAX_HISTORY_TOTAL][MAX_CMD_LEN];     /* Circular/shift ring buffer */
} SharedHistory;
```
- **Capacity**: 10,000 entries. Disk persistence backed by `~/.terminator_history`.
- **Search Algorithm**:
  1. *Phase 1 (Exact Match)*: Scans newest-to-oldest. If an exact string equality is found, returns the most recent match.
  2. *Phase 2 (Longest Common Substring - LCS)*: Computes dynamic programming LCS:
     $$\text{LCS}(s_1, s_2) = \max_{i,j} \text{suffix}(s_1[0..i], s_2[0..j])$$
     Finds the maximal LCS length $M$. If $M > 2$, returns all unique commands matching length $M$.
  3. *Phase 3 (Fallback)*: If $M \le 2$, returns `No match for search term in history`.

### 6.2 Directory Autocompletion
```c
typedef struct {
    int match_count;
    char matches[MAX_COMPLETION_MATCHES][MAX_PATH_LEN];
    char common_prefix[MAX_PATH_LEN];
    char dir_part[MAX_PATH_LEN];
    int word_start;
    int word_end;
} CompletionResult;
```
- Inspects the cursor position and extracts the word token before cursor.
- Resolves directory prefix vs filename prefix using `opendir()` and `readdir()`.
- Computes Longest Common Prefix (LCP) across all directory matches.
- If multiple matches remain ambiguous after LCP, formats a numbered menu:
  `1. abc.txt  2. abcd.txt`
  and waits for a single digit key press to complete the selection.

---

## 7. Custom Bash Script Interpreter

Instead of delegating to `/bin/bash`, Terminator implements an internal interpreter:
1. **Validation Pass**: Validates conditional syntax before execution. Detects unclosed `if` blocks, misplaced `elif` / `else`, or missing `fi` keywords, halting execution with exit status 2 on syntax errors.
2. **Conditional Stack**: Tracks nested `if` / `elif` / `else` / `fi` blocks with a frame stack tracking branch-taken flags and execution states.
3. **Condition Evaluator**: Evaluates `[ ... ]` test expressions:
   - File tests: `-f` (regular file), `-d` (directory), `-e` (exists), `-s` (non-empty).
   - String tests: `-z` (empty), `-n` (non-empty), `=`, `!=`.
   - Integer tests: `-eq`, `-ne`, `-gt`, `-ge`, `-lt`, `-le`.
4. **Variable Scoping**: Maintains local context variables (`NAME=value`), positional parameters (`$1`..`$9`, `$#`, `$0`), and `$?`.

---

## 8. Signal Handling & X11 Isolation

1. **GUI Signal Immunity**: Terminator ignores `SIGPIPE` and intercepts `SIGINT` / `SIGTSTP` at the X11 event layer. Key combinations like `Ctrl+C` or `Ctrl+Z` are translated by the GUI into targeted `kill(-pgid, sig)` calls to the active tab's foreground process group. Terminator itself never terminates on `Ctrl+C`.
2. **Tab Isolation**: Signals dispatched in Tab 1 affect only Tab 1's process group; jobs running in other tabs continue executing unhindered.
3. **X11 Thread Safety**: Only the parent GUI process interacts with Xlib. Tab threads and processes interact strictly through standard UNIX pipes and POSIX shared memory.
