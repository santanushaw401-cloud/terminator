@@ -0,0 +1,214 @@
# Terminator: A Custom Terminal

**Computing Lab (CS69201) · Term Project**  
*Terminator* is a lightweight terminal emulator and Unix shell written in C for Linux using Xlib. It renders an X11 window supporting multiple tabs, background process execution, signal dispatching, job control, persistent history search, autocompletion, parallel multi-process watching, and a custom bash script interpreter.

---

## Table of Contents
1. [Features](#features)
2. [Dependencies & Installation](#dependencies--installation)
3. [Building the Project](#building-the-project)
4. [Running Terminator](#running-terminator)
   - [Thread Mode (Default)](#thread-mode-default)
   - [Process Mode](#process-mode)
5. [User Interface & Hotkeys](#user-interface--hotkeys)
6. [Supported Commands & Syntax](#supported-commands--syntax)
   - [Built-ins](#built-ins)
   - [Multiline & Quoting](#multiline--quoting)
   - [Redirection & Pipelines](#redirection--pipelines)
   - [multiWatch](#multiwatch)
   - [Job Control](#job-control)
   - [Searchable History (Ctrl+R)](#searchable-history-ctrlr)
   - [Tab Autocompletion](#tab-autocompletion)
   - [Bash Script Execution](#bash-script-execution)
7. [Testing](#testing)
8. [Known Limitations](#known-limitations)

---

## Features

- **X11 GUI & Multi-tab Emulation**: Native window rendering using Xlib (`XOpenDisplay`, `XCreateSimpleWindow`, `XDrawString`). Displays tab titles formatted as `[ 1: ~/Downloads ] [ 2: /tmp ] [ + ]`. Tabs can be created, switched, and closed via mouse or keyboard shortcuts.
- **Non-blocking Event Loop**: Built on `select()` monitoring the X11 connection file descriptor alongside inter-process pipes, completely eliminating CPU busy-waiting.
- **Two Execution Modes**:
  - **Thread Mode (`./terminator`)**: Parent GUI process creates POSIX threads per tab, synchronized via thread-level mutexes and semaphores.
  - **Process Mode (`./terminator useProcess=True`)**: Parent GUI process forks child processes per tab, communicating through pipes with history kept in POSIX shared memory protected by a process-shared semaphore.
- **Robust Job Control**: Supports foreground commands, background commands (`&`), suspending (`Ctrl+Z`), listing (`jobs`), moving to background (`bg`), and moving to foreground (`fg`).
- **Parallel multiWatch**: `multiWatch ["cmd1", "cmd2", ...]` executes commands concurrently across independent processes and pipes, polling read ends via `select()`, and auto-repeating every 2 seconds until `Ctrl+C`.
- **Searchable History**: Up to 10,000 commands persisted to `~/.terminator_history`. `Ctrl+R` provides exact matching followed by Longest Common Substring (LCS > 2) matching.
- **Directory Autocompletion**: Tab completion supporting single match completion, Longest Common Prefix (LCP), and numbered interactive selection menus (`1. abc.txt  2. abcd.txt`).
- **Custom Bash Script Interpreter**: Self-contained interpreter for bash scripts (`./script.sh`, `bash script.sh`, `source script.sh`) supporting comments, `$VAR`, `$1..$#`, `$?`, conditionals (`if/elif/else/fi`), test operators `[ -f, -d, -eq, =, etc. ]`, and pre-execution syntax validation.

---

## Dependencies & Installation

Terminator runs on standard Linux environments (Ubuntu, Debian, Arch, Fedora) and WSL2 with an X server (WSLg, VcXsrv, or X410).

### Ubuntu / Debian:
```bash
sudo apt-get update
sudo apt-get install -y build-essential libx11-dev
```

---

## Building the Project

Compile the project using GNU Make:
```bash
make clean
make
```
This produces the `./terminator` binary using flags:
`-Wall -Wextra -O2 -Iinclude -D_GNU_SOURCE -lX11 -lpthread -lrt`

---

## Running Terminator

Ensure your `DISPLAY` environment variable is configured:
```bash
export DISPLAY=:0   # or :1 depending on your X server
```

### Thread Mode (Default)
Each tab runs as a separate POSIX thread inside a single process:
```bash
./terminator
```

### Process Mode
The GUI runs in the parent process, and each new tab forks its own independent process:
```bash
./terminator useProcess=True
```

---

## User Interface & Hotkeys

### Tab Header Navigation
- Click any tab title (e.g. `[ 1: ~/Downloads  x ]`) to switch to that tab.
- Click the red **`x`** cross sign (or middle-click the tab) to close that tab.
- Click `[ + ]` to spawn a new tab.
- `Ctrl+Shift+T`: Open a new tab.
- `Ctrl+Shift+W`: Close the active tab.
- `Ctrl+Tab` or `Ctrl+PageDown`: Switch to the next tab.
- `Ctrl+PageUp`: Switch to the previous tab.

### Bash Line-Editing Keys
Keybindings are configured by default and can be overridden via `~/.terminatorrc`:

| Shortcut | Action |
|---|---|
| `←` / `→` | Move cursor within the input line |
| `Ctrl+A` / `Ctrl+E` | Move to start / end of the line |
| `↑` / `↓` | Browse history commands |
| `Ctrl+U` / `Ctrl+K` | Delete from cursor to start / end of line |
| `Ctrl+W` | Delete the previous word |
| `Ctrl+L` | Clear the screen |
| `Ctrl+D` | Exit the shell (close tab) / send EOF |
| `Ctrl+Shift+C` | Copy selected text to X11 clipboard |
| `Ctrl+Shift+V` | Paste text from X11 clipboard |
| `Shift+PageUp` / `Shift+PageDown` | Scroll terminal buffer up / down |

### Configuration File (`~/.terminatorrc`)
A sample configuration file is provided in `sample.terminatorrc`. Terminator checks `~/.terminatorrc` on launch. If conflicting or invalid keys are detected, a warning is printed and defaults are preserved without crashes.

---

## Supported Commands & Syntax

### Built-ins
- `cd [dir]`: Changes the current working directory for the active tab (supports `~`, `-`, relative and absolute paths). Does NOT affect other tabs.
- `history [n]`: Displays the most recent `n` commands (default 1,000) with line numbers.
- `jobs`: Lists active background and suspended jobs with their process group IDs.
- `fg [%id]`: Brings a background or stopped job to the foreground.
- `bg [%id]`: Resumes a stopped job in the background.
- `source <script>` / `. <script>`: Executes a script inside the current shell tab.
- `exit [code]`: Exits the current tab shell.

### Multiline & Quoting
- Trailing backslash (`\`): Shows the continuation prompt `> ` and continues the command.
- Quotes across multiple lines: Single quotes (`'...'`) and double quotes (`"..."`) can span across lines.
- Command substitutions: `$(...)` and backticks ``` `...` ``` execute recursively (innermost first).
- Expansion rules: Single quotes are strictly literal. Double quotes expand variables (`$HOME`, `$?`, `$USER`) and command substitutions.

### Redirection & Pipelines
- `< infile`: Input redirection (verifies file existence before executing).
- `> outfile`: Output redirection (truncate).
- `>> outfile`: Output redirection (append).
- Pipelines of arbitrary depth: `cmd1 | cmd2 | ... | cmdN` with all necessary pipes.

### multiWatch
Executes multiple commands concurrently in parallel processes:
```bash
user@terminator> multiWatch ["date +%T", "ls | wc -l"]
```
Output format:
```
"date +%T", 1790860020:
----------------------------------------------------
18:37:00
----------------------------------------------------
"ls | wc -l", 1790860020:
----------------------------------------------------
3
----------------------------------------------------
```
Pressing `Ctrl+C` terminates all child processes cleanly and returns to the shell prompt.

### Job Control
```bash
user@terminator> sleep 100
^Z
[1]+ Stopped sleep 100
user@terminator> bg %1
[1]+ sleep 100 &
user@terminator> fg %1
sleep 100
^C
user@terminator>
```

### Searchable History (Ctrl+R)
Pressing `Ctrl+R` displays `Enter search term: `.
- Prints the newest exact match if present.
- Otherwise prints all commands sharing the Longest Common Substring (LCS) if length > 2.
- Otherwise displays `No match for search term in history`.

### Tab Autocompletion
Pressing `Tab` completes file names in the current tab's directory:
- 1 match: automatically completes word.
- Multiple matches: extends to longest common prefix; if ambiguous, displays a numbered list:
  ```
  1. abc.txt  2. abcd.txt
  ```
  Pressing `1` selects `abc.txt`.

### Bash Script Execution
Runs scripts directly with the built-in interpreter:
```bash
./check.sh myprog.c
bash check.sh nothere.c
```
Validates syntax (detects missing `fi`) before execution, executes conditionals (`if`, `elif`, `else`, `fi`), evaluates tests (`-f`, `-d`, `-e`, `-s`, `-z`, `-n`, `=`, `!=`, `-eq`, `-ne`, `-gt`, etc.), and handles arguments (`$1`..`$9`, `$#`, `$?`).

---

## Testing

Run the automated test suite:
```bash
make test
```
This builds and executes unit tests for parser, history, autocompletion, script runner, config loader, and job control data structures without requiring an active X11 display.

---

## Known Limitations

- Complex bash constructs such as `for` / `while` loops and shell function definitions are omitted per assignment specification.
- Rich terminal applications requiring full terminfo/pty sequences (such as `vim` or `htop`) require pseudo-terminal allocation (`openpty`), whereas standard CLI utilities (`cat`, `ls`, `gcc`, `grep`, `sleep`, `sort`, `python3`) work out of the box.
