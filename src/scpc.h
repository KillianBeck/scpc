#ifndef SCPC_H
#define SCPC_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCPC_VERSION "0.1.0"

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} StrList;

typedef enum {
    SORT_NONE = 0,
    SORT_ASC,
    SORT_DESC
} SortOrder;

typedef enum {
    OVERWRITE_PROMPT = 0,
    OVERWRITE_SKIP,
    OVERWRITE_FULL
} OverwriteMode;

typedef struct {
    int workers; /* 0 = auto */
    SortOrder sort_order;
    OverwriteMode overwrite_mode;
    bool recursive;
    bool quiet;
    bool verbose;
    bool batch_mode;
    bool preserve;
    long limit_kbps; /* 0 = unset */
    char *port;
    StrList scp_options;
    StrList ssh_options;
    StrList sources;
    char *target;
    bool has_custom_control_path;
    bool has_custom_program;
    bool show_help;
} ParsedArgs;

typedef struct {
    char *user_host;
    char *path;
} RemoteSpec;

typedef struct {
    char *source_path;
    char *rel_path;
    uint64_t size;
    bool is_remote;
    char *user_host;
} FileItem;

typedef struct {
    FileItem *files;
    size_t file_count;
    size_t file_cap;
    StrList dirs;
    uint64_t total_bytes;
    bool known_total;
    bool single_file_source;
} ScanResult;

typedef struct {
    uint64_t total_bytes;
    size_t total_files;
    size_t completed_files;
    int workers;
    bool known_total;
    bool quiet;
    double start_time;
    double last_render_time;
    uint64_t completed_bytes;
    uint64_t *active_bytes;
    size_t num_tasks;
    bool finished;
    pthread_mutex_t lock;
} ProgressTracker;

typedef struct {
    size_t files_scanned;
    size_t dirs_scanned;
    uint64_t bytes_found;
    int workers;
    bool quiet;
    double start_time;
    double last_render_time;
    size_t tick;
    bool finished;
    pthread_mutex_t lock;
} ScanProgress;

/* StrList helpers */
void strlist_init(StrList *list);
void strlist_push(StrList *list, const char *s);
void strlist_free(StrList *list);

/* cli.c */
void parsed_args_init(ParsedArgs *args);
void parsed_args_free(ParsedArgs *args);
int parse_args(int argc, char **argv, ParsedArgs *out, char *errbuf, size_t errbuf_sz);
void print_help(void);
bool is_remote_path(const char *path);
bool split_remote_path(const char *path, RemoteSpec *out);
void free_remote_spec(RemoteSpec *spec);

/* scanner.c */
void scan_result_init(ScanResult *res);
void scan_result_free(ScanResult *res);
int scan_local_sources(const StrList *sources, bool recursive, ScanResult *out,
                       char *errbuf, size_t errbuf_sz);
int scan_sources(const ParsedArgs *args, ScanResult *out,
                 char *errbuf, size_t errbuf_sz);

/* progress.c */
double now_monotonic(void);
void format_bytes(uint64_t bytes, char *buf, size_t buf_sz);
void format_speed(double bytes_per_sec, char *buf, size_t buf_sz);
void format_eta(double seconds, char *buf, size_t buf_sz);
void progress_init(ProgressTracker *pt, uint64_t total_bytes, size_t total_files,
                   int workers, bool known_total, bool quiet, size_t num_tasks);
void progress_destroy(ProgressTracker *pt);
void progress_start(ProgressTracker *pt);
void progress_update_active(ProgressTracker *pt, size_t task_id, uint64_t bytes_done);
void progress_complete_task(ProgressTracker *pt, size_t task_id,
                            uint64_t task_bytes, size_t task_files);
void progress_finish(ProgressTracker *pt, bool success);
void progress_format_line(ProgressTracker *pt, double now, char *buf, size_t buf_sz);
int64_t parse_scp_progress_line(const char *line, int64_t file_size);

void scan_progress_init(ScanProgress *sp, int workers, bool quiet);
void scan_progress_destroy(ScanProgress *sp);
void scan_progress_update(ScanProgress *sp, size_t add_files, size_t add_dirs,
                          uint64_t add_bytes);
void scan_progress_finish(ScanProgress *sp);
void scan_progress_format_line(ScanProgress *sp, double now, char *buf, size_t buf_sz);

/* workers.c */
int determine_workers(size_t file_count, uint64_t total_bytes, int cpu_count);
int get_cpu_count(void);
int run_scp_with_pty(char *const argv[], size_t task_id, int64_t file_size,
                     ProgressTracker *pt, bool forward_stdin,
                     char *err_output, size_t err_output_sz);

/* runner.c */
int run_scpc(const ParsedArgs *args);

#endif /* SCPC_H */

