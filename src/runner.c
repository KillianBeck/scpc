#include "scpc.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char *xstrdup_r(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (!copy) {
        fprintf(stderr, "scpc: out of memory\n");
        exit(1);
    }
    memcpy(copy, s, len + 1);
    return copy;
}

static char *shell_quote_r(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len * 4 + 3);
    if (!out) {
        fprintf(stderr, "scpc: out of memory\n");
        exit(1);
    }
    size_t pos = 0;
    out[pos++] = '\'';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            memcpy(out + pos, "'\\''", 4);
            pos += 4;
        } else {
            out[pos++] = s[i];
        }
    }
    out[pos++] = '\'';
    out[pos] = '\0';
    return out;
}

static int mkdir_p_local(const char *path) {
    if (!path || !*path) return 0;
    char *tmp = xstrdup_r(path);
    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

static const char *strip_first_component(const char *rel) {
    const char *slash = strchr(rel, '/');
    return slash ? slash + 1 : "";
}

static char *join_dest_path(const char *base, const char *rel) {
    if (!rel || !*rel) return xstrdup_r(base);
    size_t blen = strlen(base);
    size_t rlen = strlen(rel);
    bool need_slash = blen > 0 && base[blen - 1] != '/';
    char *out = malloc(blen + rlen + (need_slash ? 2 : 1));
    if (!out) {
        fprintf(stderr, "scpc: out of memory\n");
        exit(1);
    }
    memcpy(out, base, blen);
    size_t pos = blen;
    if (need_slash) out[pos++] = '/';
    memcpy(out + pos, rel, rlen);
    out[pos + rlen] = '\0';
    return out;
}

static char *parent_dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrdup_r("");
    if (slash == path) return xstrdup_r("/");
    size_t len = (size_t)(slash - path);
    char *out = malloc(len + 1);
    if (!out) {
        fprintf(stderr, "scpc: out of memory\n");
        exit(1);
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}

static int run_single_scp(const ParsedArgs *args, const ScanResult *scan, int workers) {
    ProgressTracker pt;
    progress_init(&pt, scan->total_bytes, scan->file_count, workers,
                  scan->known_total, args->quiet, 1);
    progress_start(&pt);

    size_t argc_total = 1 + args->scp_options.count + args->sources.count + 2;
    char **scp_argv = calloc(argc_total, sizeof(char *));
    size_t idx = 0;
    scp_argv[idx++] = "scp";
    for (size_t i = 0; i < args->scp_options.count; i++) {
        scp_argv[idx++] = args->scp_options.items[i];
    }
    for (size_t i = 0; i < args->sources.count; i++) {
        scp_argv[idx++] = args->sources.items[i];
    }
    scp_argv[idx++] = args->target;
    scp_argv[idx] = NULL;

    int64_t fsize = scan->known_total ? (int64_t)scan->total_bytes : -1;
    char err_out[4096];
    int rc = run_scp_with_pty(scp_argv, 0, fsize, &pt, true, err_out, sizeof(err_out));
    free(scp_argv);

    if (rc == 0) {
        progress_complete_task(&pt, 0, scan->known_total ? scan->total_bytes : 0,
                               scan->file_count);
    }
    progress_finish(&pt, rc == 0);

    if (rc != 0 && err_out[0] != '\0') {
        fprintf(stderr, "%s\n", err_out);
    }
    progress_destroy(&pt);
    return rc;
}

static int start_ssh_control_master(const ParsedArgs *args, const char *user_host,
                                    const char *control_path) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char cp_opt[512];
        snprintf(cp_opt, sizeof(cp_opt), "ControlPath=%s", control_path);

        size_t ssh_argc = 9 + args->ssh_options.count + 1;
        char **ssh_argv = calloc(ssh_argc, sizeof(char *));
        size_t k = 0;
        ssh_argv[k++] = "ssh";
        ssh_argv[k++] = "-MNf";
        ssh_argv[k++] = "-o";
        ssh_argv[k++] = "ControlMaster=yes";
        ssh_argv[k++] = "-o";
        ssh_argv[k++] = cp_opt;
        ssh_argv[k++] = "-o";
        ssh_argv[k++] = "ControlPersist=60";
        for (size_t i = 0; i < args->ssh_options.count; i++) {
            ssh_argv[k++] = args->ssh_options.items[i];
        }
        ssh_argv[k++] = (char *)user_host;
        ssh_argv[k] = NULL;
        execvp("ssh", ssh_argv);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static void stop_ssh_control_master(const ParsedArgs *args, const char *user_host,
                                    const char *control_path) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        char cp_opt[512];
        snprintf(cp_opt, sizeof(cp_opt), "ControlPath=%s", control_path);

        size_t ssh_argc = 7 + args->ssh_options.count + 1;
        char **ssh_argv = calloc(ssh_argc, sizeof(char *));
        size_t k = 0;
        ssh_argv[k++] = "ssh";
        ssh_argv[k++] = "-O";
        ssh_argv[k++] = "exit";
        ssh_argv[k++] = "-o";
        ssh_argv[k++] = cp_opt;
        for (size_t i = 0; i < args->ssh_options.count; i++) {
            ssh_argv[k++] = args->ssh_options.items[i];
        }
        ssh_argv[k++] = (char *)user_host;
        ssh_argv[k] = NULL;
        execvp("ssh", ssh_argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(control_path);
}

static int run_ssh_command_capture(const ParsedArgs *args, const char *user_host,
                                   const char *control_path, const char *remote_cmd,
                                   char *out, size_t out_sz) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        size_t ssh_argc = 5 + args->ssh_options.count + 1;
        char **ssh_argv = calloc(ssh_argc, sizeof(char *));
        size_t idx = 0;
        char cp_opt[512];
        ssh_argv[idx++] = "ssh";
        if (control_path) {
            snprintf(cp_opt, sizeof(cp_opt), "ControlPath=%s", control_path);
            ssh_argv[idx++] = "-o";
            ssh_argv[idx++] = cp_opt;
        }
        for (size_t i = 0; i < args->ssh_options.count; i++) {
            ssh_argv[idx++] = args->ssh_options.items[i];
        }
        ssh_argv[idx++] = (char *)user_host;
        ssh_argv[idx++] = (char *)remote_cmd;
        ssh_argv[idx] = NULL;
        execvp("ssh", ssh_argv);
        _exit(127);
    }

    close(pipefd[1]);
    size_t pos = 0;
    if (out && out_sz > 0) {
        out[0] = '\0';
        ssize_t n;
        while (pos + 1 < out_sz && (n = read(pipefd[0], out + pos, out_sz - pos - 1)) > 0) {
            pos += (size_t)n;
        }
        out[pos] = '\0';
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int prepare_remote_dirs(const ParsedArgs *args, const ScanResult *scan,
                               const char *user_host, const char *base_path,
                               bool strip_top, const char *control_path) {
    if (scan->dirs.count == 0) {
        return 0;
    }

    /* Stream NUL-separated directory paths to a single `ssh ... xargs -0 mkdir -p` */
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);

        char cp_opt[512];
        size_t ssh_argc = 6 + args->ssh_options.count + 1;
        char **ssh_argv = calloc(ssh_argc, sizeof(char *));
        size_t k = 0;
        ssh_argv[k++] = "ssh";
        if (control_path) {
            snprintf(cp_opt, sizeof(cp_opt), "ControlPath=%s", control_path);
            ssh_argv[k++] = "-o";
            ssh_argv[k++] = cp_opt;
        }
        for (size_t s = 0; s < args->ssh_options.count; s++) {
            ssh_argv[k++] = args->ssh_options.items[s];
        }
        ssh_argv[k++] = (char *)user_host;
        ssh_argv[k++] = "xargs -0 mkdir -p";
        ssh_argv[k] = NULL;
        execvp("ssh", ssh_argv);
        _exit(127);
    }

    close(pipefd[0]);
    for (size_t i = 0; i < scan->dirs.count; i++) {
        const char *rel = strip_top ? strip_first_component(scan->dirs.items[i]) : scan->dirs.items[i];
        char *full = join_dest_path(base_path, rel);
        size_t flen = strlen(full);
        if (flen > 0) {
            ssize_t w = write(pipefd[1], full, flen + 1); /* includes NUL */
            (void)w;
        }
        free(full);
    }
    close(pipefd[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Batch multiple files sharing the same destination directory into one scp call */
typedef struct {
    size_t *file_indices;
    size_t count;
    size_t cap;
    char *dst_dir;
    uint64_t total_bytes;
} TransferBatch;

static int cmp_file_by_parent_dir(const void *a, const void *b) {
    const FileItem *fa = (const FileItem *)a;
    const FileItem *fb = (const FileItem *)b;
    const char *sa = strrchr(fa->rel_path, '/');
    const char *sb = strrchr(fb->rel_path, '/');
    size_t la = sa ? (size_t)(sa - fa->rel_path) : 0;
    size_t lb = sb ? (size_t)(sb - fb->rel_path) : 0;
    size_t minl = la < lb ? la : lb;
    int c = memcmp(fa->rel_path, fb->rel_path, minl);
    if (c != 0) return c;
    if (la != lb) return la < lb ? -1 : 1;
    return strcmp(fa->rel_path, fb->rel_path);
}

static int cmp_batch_asc(const void *a, const void *b) {
    const TransferBatch *ba = (const TransferBatch *)a;
    const TransferBatch *bb = (const TransferBatch *)b;
    if (ba->total_bytes < bb->total_bytes) return -1;
    if (ba->total_bytes > bb->total_bytes) return 1;
    return 0;
}

static int cmp_batch_desc(const void *a, const void *b) {
    return cmp_batch_asc(b, a);
}

static TransferBatch *build_batches(ScanResult *scan, const char *target_base,
                                    bool target_is_remote, const char *target_user_host,
                                    bool strip_top, SortOrder sort_order,
                                    size_t *out_count) {
    /* Sort files by parent directory so files in the same dir are batched together */
    qsort(scan->files, scan->file_count, sizeof(FileItem), cmp_file_by_parent_dir);

    size_t batch_cap = 64;
    size_t batch_count = 0;
    TransferBatch *batches = calloc(batch_cap, sizeof(TransferBatch));

    const size_t max_files_per_batch = 64;
    const size_t max_arg_bytes = 24576;
    const uint64_t large_file_thresh = 10ULL * 1024ULL * 1024ULL;

    size_t i = 0;
    while (i < scan->file_count) {
        const char *rel0 = strip_top ? strip_first_component(scan->files[i].rel_path)
                                     : scan->files[i].rel_path;
        char *pdir = parent_dir_of(rel0);
        char *dst_path = join_dest_path(target_base, pdir);
        free(pdir);

        /* Ensure trailing '/' on dst_path when targeting a directory with multiple files */
        size_t dplen = strlen(dst_path);
        char *dst_arg = NULL;
        if (target_is_remote && target_user_host) {
            size_t len = strlen(target_user_host) + 1 + dplen + 2;
            dst_arg = malloc(len);
            snprintf(dst_arg, len, "%s:%s%s", target_user_host, dst_path,
                     (dplen > 0 && dst_path[dplen - 1] == '/') ? "" : "/");
        } else {
            size_t len = dplen + 2;
            dst_arg = malloc(len);
            snprintf(dst_arg, len, "%s%s", dst_path,
                     (dplen > 0 && dst_path[dplen - 1] == '/') ? "" : "/");
        }
        free(dst_path);

        if (batch_count == batch_cap) {
            batch_cap *= 2;
            batches = realloc(batches, batch_cap * sizeof(TransferBatch));
        }

        TransferBatch *b = &batches[batch_count++];
        memset(b, 0, sizeof(*b));
        b->dst_dir = dst_arg;
        size_t arg_bytes = 0;

        /* Group consecutive files with the same parent dir */
        const char *first_slash = strrchr(scan->files[i].rel_path, '/');
        size_t first_plen = first_slash ? (size_t)(first_slash - scan->files[i].rel_path) : 0;

        while (i < scan->file_count && b->count < max_files_per_batch) {
            const char *cur_rel = scan->files[i].rel_path;
            const char *cur_slash = strrchr(cur_rel, '/');
            size_t cur_plen = cur_slash ? (size_t)(cur_slash - cur_rel) : 0;

            if (b->count > 0 && (cur_plen != first_plen ||
                (first_plen > 0 && memcmp(cur_rel, scan->files[b->file_indices[0]].rel_path, first_plen) != 0))) {
                break;
            }
            /* Avoid mixing remote sources from different hosts in one batch */
            if (b->count > 0 && scan->files[i].is_remote &&
                scan->files[b->file_indices[0]].is_remote &&
                strcmp(scan->files[i].user_host, scan->files[b->file_indices[0]].user_host) != 0) {
                break;
            }

            size_t slen = strlen(scan->files[i].source_path) + 32;
            if (b->count > 0 && arg_bytes + slen > max_arg_bytes) {
                break;
            }
            if (sort_order != SORT_NONE && b->count > 0 &&
                (scan->files[i].size >= large_file_thresh ||
                 scan->files[b->file_indices[0]].size >= large_file_thresh)) {
                break;
            }

            if (b->count == b->cap) {
                b->cap = b->cap == 0 ? 16 : b->cap * 2;
                b->file_indices = realloc(b->file_indices, b->cap * sizeof(size_t));
            }
            b->file_indices[b->count++] = i;
            b->total_bytes += scan->files[i].size;
            arg_bytes += slen;
            i++;
        }
    }

    if (sort_order == SORT_ASC && batch_count > 1) {
        qsort(batches, batch_count, sizeof(TransferBatch), cmp_batch_asc);
    } else if (sort_order == SORT_DESC && batch_count > 1) {
        qsort(batches, batch_count, sizeof(TransferBatch), cmp_batch_desc);
    }

    *out_count = batch_count;
    return batches;
}

static void free_batches(TransferBatch *batches, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(batches[i].file_indices);
        free(batches[i].dst_dir);
    }
    free(batches);
}

/* Open-addressed hash set of existing destination paths for O(1) lookup */
typedef struct {
    char **keys;
    size_t cap;
    size_t count;
} PathSet;

static uint64_t hash_str_fnv(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static void path_set_init(PathSet *set) {
    set->cap = 1024;
    set->count = 0;
    set->keys = calloc(set->cap, sizeof(char *));
}

static void path_set_free(PathSet *set) {
    for (size_t i = 0; i < set->cap; i++) {
        free(set->keys[i]);
    }
    free(set->keys);
    set->keys = NULL;
    set->cap = 0;
    set->count = 0;
}

static void path_set_grow(PathSet *set) {
    size_t new_cap = set->cap * 2;
    char **new_keys = calloc(new_cap, sizeof(char *));
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < set->cap; i++) {
        if (set->keys[i]) {
            size_t idx = (size_t)hash_str_fnv(set->keys[i]) & mask;
            while (new_keys[idx]) idx = (idx + 1) & mask;
            new_keys[idx] = set->keys[i];
        }
    }
    free(set->keys);
    set->keys = new_keys;
    set->cap = new_cap;
}

static void path_set_insert(PathSet *set, const char *key) {
    if ((set->count + 1) * 10 >= set->cap * 7) {
        path_set_grow(set);
    }
    size_t mask = set->cap - 1;
    size_t idx = (size_t)hash_str_fnv(key) & mask;
    while (set->keys[idx]) {
        if (strcmp(set->keys[idx], key) == 0) return;
        idx = (idx + 1) & mask;
    }
    set->keys[idx] = xstrdup_r(key);
    set->count++;
}

static bool path_set_contains(const PathSet *set, const char *key) {
    if (set->count == 0) return false;
    size_t mask = set->cap - 1;
    size_t idx = (size_t)hash_str_fnv(key) & mask;
    while (set->keys[idx]) {
        if (strcmp(set->keys[idx], key) == 0) return true;
        idx = (idx + 1) & mask;
    }
    return false;
}

static void collect_remote_existing(const ParsedArgs *args, const char *user_host,
                                    const char *base_path, bool strip_top,
                                    const ScanResult *scan, const char *control_path,
                                    PathSet *out) {
    /* Scope `find` to the actual target roots instead of scanning all of base_path (e.g. '.'). */
    StrList roots;
    strlist_init(&roots);

    if (strip_top) {
        strlist_push(&roots, base_path);
    } else {
        for (size_t i = 0; i < args->sources.count; i++) {
            const char *s = args->sources.items[i];
            size_t len = strlen(s);
            while (len > 1 && s[len - 1] == '/') len--;
            const char *slash = NULL;
            for (size_t k = len; k > 0; k--) {
                if (s[k - 1] == '/') {
                    slash = s + k;
                    break;
                }
            }
            const char *bname = slash ? slash : s;
            size_t blen = slash ? (size_t)((s + len) - slash) : len;
            char *rel = malloc(blen + 1);
            memcpy(rel, bname, blen);
            rel[blen] = '\0';
            char *full = join_dest_path(base_path, rel);
            free(rel);
            bool seen = false;
            for (size_t j = 0; j < roots.count; j++) {
                if (strcmp(roots.items[j], full) == 0) {
                    seen = true;
                    break;
                }
            }
            if (!seen) strlist_push(&roots, full);
            free(full);
        }
        if (roots.count == 0 && scan->file_count > 0) {
            strlist_push(&roots, base_path);
        }
    }

    size_t cmd_cap = 256;
    for (size_t i = 0; i < roots.count; i++) {
        cmd_cap += strlen(roots.items[i]) * 4 + 8;
    }
    char *remote_cmd = malloc(cmd_cap);
    size_t pos = (size_t)snprintf(remote_cmd, cmd_cap, "find");
    for (size_t i = 0; i < roots.count; i++) {
        char *q = shell_quote_r(roots.items[i]);
        pos += (size_t)snprintf(remote_cmd + pos, cmd_cap - pos, " %s", q);
        free(q);
    }
    snprintf(remote_cmd + pos, cmd_cap - pos, " -type f 2>/dev/null || true");
    strlist_free(&roots);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        free(remote_cmd);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        char cp_opt[512];
        size_t ssh_argc = 6 + args->ssh_options.count + 1;
        char **ssh_argv = calloc(ssh_argc, sizeof(char *));
        size_t k = 0;
        ssh_argv[k++] = "ssh";
        if (control_path) {
            snprintf(cp_opt, sizeof(cp_opt), "ControlPath=%s", control_path);
            ssh_argv[k++] = "-o";
            ssh_argv[k++] = cp_opt;
        }
        for (size_t i = 0; i < args->ssh_options.count; i++) {
            ssh_argv[k++] = args->ssh_options.items[i];
        }
        ssh_argv[k++] = (char *)user_host;
        ssh_argv[k++] = remote_cmd;
        ssh_argv[k] = NULL;
        execvp("ssh", ssh_argv);
        _exit(127);
    }

    close(pipefd[1]);
    free(remote_cmd);

    FILE *fp = fdopen(pipefd[0], "r");
    char *line = NULL;
    size_t cap = 0;
    ssize_t nread;
    while (fp && (nread = getline(&line, &cap, fp)) != -1) {
        while (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r')) {
            line[--nread] = '\0';
        }
        if (nread > 0) {
            /* Normalize leading "./" when base_path is "." */
            const char *norm = line;
            if (norm[0] == '.' && norm[1] == '/') norm += 2;
            path_set_insert(out, norm);
        }
    }
    free(line);
    if (fp) fclose(fp);

    int status = 0;
    waitpid(pid, &status, 0);
}

static void make_remote_files_writable(const ParsedArgs *args, const char *user_host,
                                       const char *control_path, const ScanResult *skipped,
                                       const char *base_path, bool strip_top) {
    if (skipped->file_count == 0) return;

    int pipefd[2];
    if (pipe(pipefd) != 0) return;

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        char cp_opt[512];
        size_t ssh_argc = 6 + args->ssh_options.count + 1;
        char **ssh_argv = calloc(ssh_argc, sizeof(char *));
        size_t k = 0;
        ssh_argv[k++] = "ssh";
        if (control_path) {
            snprintf(cp_opt, sizeof(cp_opt), "ControlPath=%s", control_path);
            ssh_argv[k++] = "-o";
            ssh_argv[k++] = cp_opt;
        }
        for (size_t i = 0; i < args->ssh_options.count; i++) {
            ssh_argv[k++] = args->ssh_options.items[i];
        }
        ssh_argv[k++] = (char *)user_host;
        ssh_argv[k++] = "xargs -0 chmod u+w 2>/dev/null || true";
        ssh_argv[k] = NULL;
        execvp("ssh", ssh_argv);
        _exit(127);
    }

    close(pipefd[0]);
    for (size_t i = 0; i < skipped->file_count; i++) {
        const char *rel = strip_top ? strip_first_component(skipped->files[i].rel_path)
                                    : skipped->files[i].rel_path;
        char *full = join_dest_path(base_path, rel);
        ssize_t w = write(pipefd[1], full, strlen(full) + 1);
        (void)w;
        free(full);
    }
    close(pipefd[1]);

    int status = 0;
    waitpid(pid, &status, 0);
}

static void make_local_files_writable(const ScanResult *skipped, const char *base_path,
                                      bool strip_top) {
    for (size_t i = 0; i < skipped->file_count; i++) {
        const char *rel = strip_top ? strip_first_component(skipped->files[i].rel_path)
                                    : skipped->files[i].rel_path;
        char *full = join_dest_path(base_path, rel);
        struct stat st;
        if (stat(full, &st) == 0 && (st.st_mode & S_IWUSR) == 0) {
            chmod(full, st.st_mode | S_IWUSR);
        }
        free(full);
    }
}

typedef struct {
    const ParsedArgs *args;
    const ScanResult *scan;
    const TransferBatch *batches;
    size_t batch_count;
    ProgressTracker *pt;
    const char *control_path;
    long per_worker_limit;

    pthread_mutex_t queue_lock;
    size_t next_idx;
    int first_error;
    char error_msg[2048];
} WorkerPoolCtx;

static void *worker_thread_fn(void *arg) {
    WorkerPoolCtx *ctx = (WorkerPoolCtx *)arg;

    for (;;) {
        pthread_mutex_lock(&ctx->queue_lock);
        if (ctx->first_error != 0 || ctx->next_idx >= ctx->batch_count) {
            pthread_mutex_unlock(&ctx->queue_lock);
            break;
        }
        size_t batch_id = ctx->next_idx++;
        pthread_mutex_unlock(&ctx->queue_lock);

        const TransferBatch *batch = &ctx->batches[batch_id];

        StrList cmd;
        strlist_init(&cmd);
        strlist_push(&cmd, "scp");
        for (size_t i = 0; i < ctx->args->scp_options.count; i++) {
            const char *opt = ctx->args->scp_options.items[i];
            if (strcmp(opt, "-r") == 0) {
                continue;
            }
            if (strcmp(opt, "-l") == 0 && i + 1 < ctx->args->scp_options.count) {
                i++;
                char lbuf[32];
                snprintf(lbuf, sizeof(lbuf), "%ld", ctx->per_worker_limit);
                strlist_push(&cmd, "-l");
                strlist_push(&cmd, lbuf);
                continue;
            }
            strlist_push(&cmd, opt);
        }
        if (ctx->control_path) {
            char cp_opt[512];
            snprintf(cp_opt, sizeof(cp_opt), "ControlPath=%s", ctx->control_path);
            strlist_push(&cmd, "-o");
            strlist_push(&cmd, "ControlMaster=auto");
            strlist_push(&cmd, "-o");
            strlist_push(&cmd, cp_opt);
        }

        for (size_t j = 0; j < batch->count; j++) {
            const FileItem *item = &ctx->scan->files[batch->file_indices[j]];
            if (item->is_remote && item->user_host) {
                size_t slen = strlen(item->user_host) + 1 + strlen(item->source_path) + 1;
                char *src_arg = malloc(slen);
                snprintf(src_arg, slen, "%s:%s", item->user_host, item->source_path);
                strlist_push(&cmd, src_arg);
                free(src_arg);
            } else {
                strlist_push(&cmd, item->source_path);
            }
        }
        strlist_push(&cmd, batch->dst_dir);

        char **argv = calloc(cmd.count + 1, sizeof(char *));
        for (size_t i = 0; i < cmd.count; i++) {
            argv[i] = cmd.items[i];
        }
        argv[cmd.count] = NULL;

        char err_out[2048];
        int rc = run_scp_with_pty(argv, batch_id, (int64_t)batch->total_bytes,
                                  ctx->pt, false, err_out, sizeof(err_out));

        free(argv);
        strlist_free(&cmd);

        if (rc != 0) {
            pthread_mutex_lock(&ctx->queue_lock);
            if (ctx->first_error == 0) {
                ctx->first_error = rc;
                snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", err_out);
            }
            pthread_mutex_unlock(&ctx->queue_lock);
            break;
        }

        progress_complete_task(ctx->pt, batch_id, batch->total_bytes, batch->count);
    }

    return NULL;
}

static void add_to_scan(ScanResult *dst, const FileItem *src_item) {
    if (dst->file_count == dst->file_cap) {
        size_t new_cap = dst->file_cap == 0 ? 64 : dst->file_cap * 2;
        dst->files = realloc(dst->files, new_cap * sizeof(FileItem));
        dst->file_cap = new_cap;
    }
    FileItem *item = &dst->files[dst->file_count++];
    item->source_path = xstrdup_r(src_item->source_path);
    item->rel_path = xstrdup_r(src_item->rel_path);
    item->size = src_item->size;
    item->is_remote = src_item->is_remote;
    item->user_host = xstrdup_r(src_item->user_host);
    dst->total_bytes += src_item->size;
}

static int execute_transfer_pass(const ParsedArgs *args, ScanResult *pass_scan,
                                 const char *base_path, bool target_remote,
                                 const char *target_user_host, bool strip_top_dir,
                                 int workers, const char *active_ctl) {
    if (pass_scan->file_count == 0) return 0;

    size_t batch_count = 0;
    TransferBatch *batches = build_batches(pass_scan, base_path, target_remote,
                                           target_user_host, strip_top_dir,
                                           args->sort_order, &batch_count);

    int pass_workers = workers;
    if ((size_t)pass_workers > batch_count && batch_count > 0) {
        pass_workers = (int)batch_count;
    }

    ProgressTracker pt;
    progress_init(&pt, pass_scan->total_bytes, pass_scan->file_count, pass_workers,
                  true, args->quiet, batch_count);
    progress_start(&pt);

    long per_worker_limit = 0;
    if (args->limit_kbps > 0) {
        per_worker_limit = args->limit_kbps / pass_workers;
        if (per_worker_limit < 1) per_worker_limit = 1;
    }

    WorkerPoolCtx ctx = {
        .args = args,
        .scan = pass_scan,
        .batches = batches,
        .batch_count = batch_count,
        .pt = &pt,
        .control_path = active_ctl,
        .per_worker_limit = per_worker_limit,
        .next_idx = 0,
        .first_error = 0,
    };
    ctx.error_msg[0] = '\0';
    pthread_mutex_init(&ctx.queue_lock, NULL);

    pthread_t *threads = calloc((size_t)pass_workers, sizeof(pthread_t));
    for (int i = 0; i < pass_workers; i++) {
        pthread_create(&threads[i], NULL, worker_thread_fn, &ctx);
    }
    for (int i = 0; i < pass_workers; i++) {
        pthread_join(threads[i], NULL);
    }
    free(threads);
    pthread_mutex_destroy(&ctx.queue_lock);

    progress_finish(&pt, ctx.first_error == 0);
    progress_destroy(&pt);

    if (ctx.first_error != 0 && ctx.error_msg[0] != '\0') {
        fprintf(stderr, "%s\n", ctx.error_msg);
    }

    free_batches(batches, batch_count);
    return ctx.first_error;
}

static bool prompt_overwrite_skipped(size_t skipped_count, uint64_t skipped_bytes) {
    char size_str[32];
    format_bytes(skipped_bytes, size_str, sizeof(size_str));
    fprintf(stderr, "Skipped %zu existing files (%s). Overwrite them? [y/N]: ",
            skipped_count, size_str);
    fflush(stderr);

    char resp[32];
    if (!fgets(resp, sizeof(resp), stdin)) {
        return false;
    }
    const char *p = resp;
    while (*p && isspace((unsigned char)*p)) p++;
    return (*p == 'y' || *p == 'Y');
}

int run_scpc(const ParsedArgs *args) {
    ScanResult scan;
    char errbuf[1024] = {0};
    if (scan_sources(args, &scan, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "scpc: %s\n", errbuf);
        return 1;
    }

    int workers = args->workers > 0
                      ? args->workers
                      : determine_workers(scan.file_count, scan.total_bytes, get_cpu_count());
    if (scan.known_total && scan.file_count > 0 && (size_t)workers > scan.file_count) {
        workers = (int)scan.file_count;
    }

    bool target_remote = is_remote_path(args->target);
    bool any_source_remote = scan.file_count > 0 && scan.files[0].is_remote;

    if (!scan.known_total || scan.file_count <= 1 ||
        args->has_custom_program || (target_remote && any_source_remote)) {
        int rc = run_single_scp(args, &scan, workers);
        scan_result_free(&scan);
        return rc;
    }

    RemoteSpec target_spec = {0};
    const char *base_path = args->target;
    if (target_remote) {
        split_remote_path(args->target, &target_spec);
        base_path = target_spec.path;
    }

    bool strip_top_dir = false;
    bool target_ends_slash = strlen(base_path) > 0 && base_path[strlen(base_path) - 1] == '/';
    bool single_dir_source = (args->sources.count == 1 && !scan.single_file_source);

    char control_path[256] = {0};
    const char *active_ctl = NULL;
    const char *ctl_host = target_remote ? target_spec.user_host
                                         : (any_source_remote ? scan.files[0].user_host : NULL);

    if (ctl_host && !args->has_custom_control_path) {
        snprintf(control_path, sizeof(control_path), "/tmp/scpc_ctl_%d", (int)getpid());
        if (start_ssh_control_master(args, ctl_host, control_path) == 0) {
            active_ctl = control_path;
        }
    }

    if (target_remote) {
        if (single_dir_source && !target_ends_slash) {
            char *qbase = shell_quote_r(base_path);
            size_t clen = strlen(qbase) + 32;
            char *chk = malloc(clen);
            snprintf(chk, clen, "test -d %s && echo Y || echo N", qbase);
            free(qbase);
            char resp[32] = {0};
            if (run_ssh_command_capture(args, target_spec.user_host, active_ctl, chk,
                                        resp, sizeof(resp)) == 0 &&
                resp[0] == 'N') {
                strip_top_dir = true;
            }
            free(chk);
        }
    } else {
        struct stat tst;
        bool exists_dir = (stat(base_path, &tst) == 0 && S_ISDIR(tst.st_mode));
        if (single_dir_source && !target_ends_slash && !exists_dir) {
            strip_top_dir = true;
        }
        for (size_t i = 0; i < scan.dirs.count; i++) {
            const char *rel = strip_top_dir ? strip_first_component(scan.dirs.items[i]) : scan.dirs.items[i];
            char *full = join_dest_path(base_path, rel);
            mkdir_p_local(full);
            free(full);
        }
    }

    int rc = 0;
    if (args->overwrite_mode == OVERWRITE_FULL) {
        if (target_remote) {
            if (prepare_remote_dirs(args, &scan, target_spec.user_host, base_path,
                                    strip_top_dir, active_ctl) != 0) {
                if (active_ctl && ctl_host) {
                    stop_ssh_control_master(args, ctl_host, active_ctl);
                }
                free_remote_spec(&target_spec);
                rc = run_single_scp(args, &scan, 1);
                scan_result_free(&scan);
                return rc;
            }
            make_remote_files_writable(args, target_spec.user_host, active_ctl,
                                       &scan, base_path, strip_top_dir);
        } else {
            make_local_files_writable(&scan, base_path, strip_top_dir);
        }
        rc = execute_transfer_pass(args, &scan, base_path, target_remote,
                                   target_spec.user_host, strip_top_dir,
                                   workers, active_ctl);
        if (active_ctl && ctl_host) {
            stop_ssh_control_master(args, ctl_host, active_ctl);
        }
        free_remote_spec(&target_spec);
        scan_result_free(&scan);
        return rc;
    }

    /* Partition files into to_transfer vs skipped (already existing at destination) */
    ScanResult to_transfer, skipped;
    scan_result_init(&to_transfer);
    scan_result_init(&skipped);

    if (target_remote) {
        PathSet existing;
        path_set_init(&existing);
        collect_remote_existing(args, target_spec.user_host, base_path,
                                strip_top_dir, &scan, active_ctl, &existing);

        for (size_t i = 0; i < scan.file_count; i++) {
            const char *rel = strip_top_dir ? strip_first_component(scan.files[i].rel_path)
                                            : scan.files[i].rel_path;
            char *full = join_dest_path(base_path, rel);
            const char *norm = full;
            if (norm[0] == '.' && norm[1] == '/') norm += 2;
            if (path_set_contains(&existing, norm)) {
                add_to_scan(&skipped, &scan.files[i]);
            } else {
                add_to_scan(&to_transfer, &scan.files[i]);
            }
            free(full);
        }
        path_set_free(&existing);
    } else {
        for (size_t i = 0; i < scan.file_count; i++) {
            const char *rel = strip_top_dir ? strip_first_component(scan.files[i].rel_path)
                                            : scan.files[i].rel_path;
            char *full = join_dest_path(base_path, rel);
            struct stat fst;
            if (stat(full, &fst) == 0 && S_ISREG(fst.st_mode)) {
                add_to_scan(&skipped, &scan.files[i]);
            } else {
                add_to_scan(&to_transfer, &scan.files[i]);
            }
            free(full);
        }
    }

    if (skipped.file_count > 0 && !args->quiet) {
        char sbuf[32];
        format_bytes(skipped.total_bytes, sbuf, sizeof(sbuf));
        fprintf(stderr, "Skipping %zu existing files (%s); %zu remaining to transfer.\n",
                skipped.file_count, sbuf, to_transfer.file_count);
    }

    if (to_transfer.file_count > 0) {
        if (target_remote &&
            prepare_remote_dirs(args, &scan, target_spec.user_host, base_path,
                                strip_top_dir, active_ctl) != 0) {
            if (active_ctl && ctl_host) {
                stop_ssh_control_master(args, ctl_host, active_ctl);
            }
            scan_result_free(&to_transfer);
            scan_result_free(&skipped);
            free_remote_spec(&target_spec);
            rc = run_single_scp(args, &scan, 1);
            scan_result_free(&scan);
            return rc;
        }
        rc = execute_transfer_pass(args, &to_transfer, base_path, target_remote,
                                   target_spec.user_host, strip_top_dir,
                                   workers, active_ctl);
    }

    if (rc == 0 && skipped.file_count > 0 &&
        args->overwrite_mode == OVERWRITE_PROMPT &&
        !args->quiet && !args->batch_mode && isatty(STDIN_FILENO)) {
        if (prompt_overwrite_skipped(skipped.file_count, skipped.total_bytes)) {
            if (target_remote) {
                make_remote_files_writable(args, target_spec.user_host, active_ctl,
                                           &skipped, base_path, strip_top_dir);
            } else {
                make_local_files_writable(&skipped, base_path, strip_top_dir);
            }
            rc = execute_transfer_pass(args, &skipped, base_path, target_remote,
                                       target_spec.user_host, strip_top_dir,
                                       workers, active_ctl);
        }
    }

    if (active_ctl && ctl_host) {
        stop_ssh_control_master(args, ctl_host, active_ctl);
    }

    scan_result_free(&to_transfer);
    scan_result_free(&skipped);
    free_remote_spec(&target_spec);
    scan_result_free(&scan);
    return rc;
}
