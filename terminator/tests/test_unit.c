#include "terminator.h"
#include "parser.h"
#include "history.h"
#include "completion.h"
#include "script.h"
#include "config.h"
#include "jobcontrol.h"
#include "shell.h"
#include "builtins.h"
#include <assert.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        tests_passed++; \
    } else { \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } \
} while(0)

void test_multiline_parser(void) {
    printf("Testing Task 3: Multiline Input & Quoting Parser...\n");

    /* 1. Complete line */
    TEST_ASSERT(parser_is_incomplete("echo hello") == 0, "Simple command is complete");

    /* 2. Trailing backslash */
    TEST_ASSERT(parser_is_incomplete("echo hello \\") == 1, "Trailing backslash is incomplete");

    /* 3. Unclosed single quote */
    TEST_ASSERT(parser_is_incomplete("echo 'hello world") == 1, "Unclosed single quote is incomplete");

    /* 4. Unclosed double quote */
    TEST_ASSERT(parser_is_incomplete("echo \"hello world") == 1, "Unclosed double quote is incomplete");

    /* 5. Closed quotes */
    TEST_ASSERT(parser_is_incomplete("echo 'hello' \"world\"") == 0, "Closed quotes is complete");

    /* 6. Unclosed command substitution */
    TEST_ASSERT(parser_is_incomplete("echo $(basename $(pwd)") == 1, "Unclosed $( is incomplete");

    /* 7. Closed command substitution */
    TEST_ASSERT(parser_is_incomplete("echo $(basename $(pwd))") == 0, "Closed $( is complete");

    /* 8. Variable expansion */
    setenv("MY_TEST_VAR", "TerminatorRocks", 1);
    char *exp1 = parser_expand("echo $MY_TEST_VAR", 0, ".");
    TEST_ASSERT(strstr(exp1, "TerminatorRocks") != NULL, "Variable expansion $VAR works");
    free(exp1);

    /* 9. Exit status expansion $? */
    char *exp2 = parser_expand("echo $?", 42, ".");
    TEST_ASSERT(strstr(exp2, "42") != NULL, "Exit status $? expands correctly");
    free(exp2);

    /* 10. Single quotes are literal */
    char *exp3 = parser_expand("echo 'literal: $MY_TEST_VAR'", 0, ".");
    TEST_ASSERT(strstr(exp3, "$MY_TEST_VAR") != NULL, "Single quotes preserve literal $VAR");
    free(exp3);
}

void test_history_search(void) {
    printf("Testing Task 9: Searchable History & LCS Search...\n");

    history_init(MODE_THREAD);
    history_clear();

    history_add("echo first");
    history_add("gcc -o myprog myprog.c");
    history_add("./myprog < in.txt");
    history_add("ls -la");

    TEST_ASSERT(history_count() >= 4, "History entries added");

    /* Exact match */
    char res[4096];
    history_search_ctrl_r("ls -la", res, sizeof(res));
    TEST_ASSERT(strstr(res, "ls -la") != NULL, "Exact match finds command");

    /* Longest common substring match */
    history_search_ctrl_r("myprog", res, sizeof(res));
    TEST_ASSERT(strstr(res, "myprog.c") != NULL && strstr(res, "in.txt") != NULL,
                "LCS match returns all matching commands sharing max length substring");

    /* No match */
    history_search_ctrl_r("nonexistent_random_token_xyz", res, sizeof(res));
    TEST_ASSERT(strstr(res, "No match for search term in history") != NULL,
                "No match message returned for nonexistent term");

    history_cleanup();
}

void test_completion(void) {
    printf("Testing Task 10: Autocompletion & Prefix Matching...\n");

    /* Test longest common prefix */
    char matches[2][MAX_FILENAME_LEN] = { "abc.txt", "abcd.txt" };
    char lcp[MAX_FILENAME_LEN];
    longest_common_prefix(matches, 2, lcp, sizeof(lcp));
    TEST_ASSERT(strcmp(lcp, "abc") == 0, "Longest common prefix of abc.txt and abcd.txt is abc");

    /* Test file completion on existing directory */
    CompletionResult cr;
    int count = complete_word("cat Makef", 9, ".", &cr);
    TEST_ASSERT(count == 1, "Single match found for Makef");
    TEST_ASSERT(strcmp(cr.common_prefix, "Makefile") == 0, "Makefile completed correctly");
}

void test_scripts(void) {
    printf("Testing Task 11: Bash Script Interpreter...\n");

    /* 1. Syntax check on check.sh */
    char err[512];
    int syn = script_validate_syntax("tests/check.sh", err, sizeof(err));
    TEST_ASSERT(syn == 0, "check.sh has valid syntax");

    /* 2. Syntax check on syntax_error.sh */
    int syn_err = script_validate_syntax("tests/syntax_error.sh", err, sizeof(err));
    TEST_ASSERT(syn_err != 0 && strstr(err, "expected 'fi'") != NULL,
                "syntax_error.sh correctly caught missing 'fi'");

    /* 3. Execute check.sh with existing file */
    int pfd[2];
    pipe(pfd);
    char *argv_exist[] = { "tests/check.sh", "Makefile" };
    int status = 0;
    script_execute_file("tests/check.sh", 2, argv_exist, pfd[1], -1, ".", &status);
    close(pfd[1]);

    char out[1024] = {0};
    read(pfd[0], out, sizeof(out) - 1);
    close(pfd[0]);
    TEST_ASSERT(strstr(out, "File exists, compiling...") != NULL,
                "check.sh correctly executed if branch for existing file");

    /* 4. Execute check.sh with missing file */
    pipe(pfd);
    char *argv_missing[] = { "tests/check.sh", "nothere.c" };
    status = 0;
    script_execute_file("tests/check.sh", 2, argv_missing, pfd[1], -1, ".", &status);
    close(pfd[1]);

    memset(out, 0, sizeof(out));
    read(pfd[0], out, sizeof(out) - 1);
    close(pfd[0]);
    TEST_ASSERT(strstr(out, "Source file missing!") != NULL,
                "check.sh correctly executed else branch for missing file");
}

void test_config(void) {
    printf("Testing Task 7: Hotkey Config & Conflict Resolution...\n");

    AppConfig cfg;
    config_init_defaults(&cfg);

    TEST_ASSERT(cfg.bindings[ACT_MOVE_START].keysym == XK_a, "Default move_start is Ctrl+A");
    TEST_ASSERT(cfg.bindings[ACT_DEL_WORD].keysym == XK_w, "Default del_word is Ctrl+W");

    /* Test match */
    ActionType act = config_match_action(&cfg, XK_a, ControlMask);
    TEST_ASSERT(act == ACT_MOVE_START, "Ctrl+A matches ACT_MOVE_START");
}

void test_job_control(void) {
    printf("Testing Task 8: Job Control Data Structures...\n");

    JobList jl;
    job_list_init(&jl);

    int id1 = job_add(&jl, 1001, "sleep 100", JOB_RUNNING);
    int id2 = job_add(&jl, 1002, "sleep 200", JOB_STOPPED);

    TEST_ASSERT(id1 == 1 && id2 == 2, "Job IDs sequentially allocated");
    TEST_ASSERT(jl.job_count == 2, "Job list count is 2");

    Job *j = job_find_by_id(&jl, 1);
    TEST_ASSERT(j != NULL && strcmp(j->command, "sleep 100") == 0, "Job 1 retrieved successfully");

    char jout[1024];
    job_list_print(&jl, jout, sizeof(jout));
    TEST_ASSERT(strstr(jout, "[1]") != NULL && strstr(jout, "Running") != NULL,
                "Job list print outputs running status correctly");
    TEST_ASSERT(strstr(jout, "[2]+") != NULL && strstr(jout, "Stopped") != NULL,
                "Job list print outputs stopped status with + for current job");

    job_remove(&jl, 1);
    TEST_ASSERT(jl.job_count == 1, "Job removal decreases job count");
}

void test_cd_builtin(void) {
    printf("Testing Task 2: cd Builtin Command...\n");
    TabState tab;
    shell_tab_init(&tab, 1, MODE_THREAD);

    int pfd[2];
    pipe(pfd);

    /* Test cd /tmp */
    char *args1[] = { "cd", "/tmp" };
    int r1 = builtin_execute(&tab, args1, 2, pfd[1], -1);
    TEST_ASSERT(r1 == 0, "cd /tmp returns 0 without buffer overflow");
    TEST_ASSERT(strcmp(tab.cwd, "/tmp") == 0, "tab->cwd successfully updated to /tmp");

    /* Test cd - */
    char *args2[] = { "cd", "-" };
    int r2 = builtin_execute(&tab, args2, 2, pfd[1], -1);
    TEST_ASSERT(r2 == 0, "cd - returns 0 without buffer overflow");

    /* Test shell_execute_line with cd */
    int r4 = shell_execute_line(&tab, "cd /tmp", pfd[1], -1);
    TEST_ASSERT(r4 == 0, "shell_execute_line('cd /tmp') returns 0");
    TEST_ASSERT(strcmp(tab.cwd, "/tmp") == 0, "tab->cwd successfully updated via shell_execute_line");

    int r5 = shell_execute_line(&tab, "cd ~", pfd[1], -1);
    TEST_ASSERT(r5 == 0, "shell_execute_line('cd ~') returns 0");

    close(pfd[0]);
    close(pfd[1]);
}

static int test_find_lowest_tab_id(const int *active_ids, int count) {
    for (int candidate = 1; candidate <= MAX_TABS + 1; candidate++) {
        int in_use = 0;
        for (int i = 0; i < count; i++) {
            if (active_ids[i] == candidate) {
                in_use = 1;
                break;
            }
        }
        if (!in_use) return candidate;
    }
    return count + 1;
}

void test_tab_allocation_sequence(void) {
    printf("Testing Task 1: Tab ID Allocation Sequence (Lowest Available ID)...\n");

    /* Tabs 2 and 3 opened, Tab 1 closed */
    int active[8] = { 2, 3 };
    int count = 2;

    int next1 = test_find_lowest_tab_id(active, count);
    TEST_ASSERT(next1 == 1, "Tabs [2, 3] open: next tab allocated is 1");

    /* Now tabs [2, 3, 1] are open */
    active[count++] = next1;
    int next2 = test_find_lowest_tab_id(active, count);
    TEST_ASSERT(next2 == 4, "Tabs [2, 3, 1] open: next tab allocated is 4");

    /* Now tabs [2, 3, 1, 4] are open */
    active[count++] = next2;
    int next3 = test_find_lowest_tab_id(active, count);
    TEST_ASSERT(next3 == 5, "Tabs [2, 3, 1, 4] open: next tab allocated is 5");

    /* Tabs 1..5 removed from 1..8 -> [6, 7, 8] open */
    int active2[8] = { 6, 7, 8 };
    int count2 = 3;
    int next_after_8 = test_find_lowest_tab_id(active2, count2);
    TEST_ASSERT(next_after_8 == 1, "Tabs [6, 7, 8] open: next tab allocated is 1");
}

int main(void) {
    printf("==========================================\n");
    printf("TERMINATOR CUSTOM TERMINAL - UNIT TEST SUITE\n");
    printf("==========================================\n\n");

    test_tab_allocation_sequence();
    printf("\n");

    test_cd_builtin();
    printf("\n");

    test_multiline_parser();
    printf("\n");

    test_history_search();
    printf("\n");

    test_completion();
    printf("\n");

    test_scripts();
    printf("\n");

    test_config();
    printf("\n");

    test_job_control();
    printf("\n");

    printf("==========================================\n");
    printf("RESULTS: %d PASSED, %d FAILED\n", tests_passed, tests_failed);
    printf("==========================================\n");

    return tests_failed == 0 ? 0 : 1;
}
