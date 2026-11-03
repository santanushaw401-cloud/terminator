#ifndef JOBCONTROL_H
#define JOBCONTROL_H

#include "terminator.h"

typedef enum {
    JOB_RUNNING = 0,
    JOB_STOPPED,
    JOB_DONE
} JobState;

typedef struct {
    int id;
    pid_t pgid;
    char command[MAX_CMD_LEN];
    JobState state;
} Job;

typedef struct {
    Job jobs[MAX_JOBS];
    int job_count;
    int next_id;
    pid_t fg_pgid;
    char fg_cmd[MAX_CMD_LEN];
} JobList;

void job_list_init(JobList *jl);
int job_add(JobList *jl, pid_t pgid, const char *cmd, JobState state);
Job *job_find_by_id(JobList *jl, int id);
Job *job_find_by_pgid(JobList *jl, pid_t pgid);
Job *job_get_current(JobList *jl);
void job_remove(JobList *jl, int id);
void job_check_status(JobList *jl, char *notify_buf, int max_len);
void job_list_print(const JobList *jl, char *out_buf, int max_len);
int job_fg(JobList *jl, int id, char *out_buf, int max_len, pid_t *out_fg_pgid, char *out_fg_cmd);
int job_bg(JobList *jl, int id, char *out_buf, int max_len);

#endif /* JOBCONTROL_H */
