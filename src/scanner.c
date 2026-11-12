#include "scpc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static char *xstrdup_s(const char *s) {
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

void scan_result_init(ScanResult *res) {
    memset(res, 0, sizeof(*res));
    strlist_init(&res->dirs);
    res->known_total = true;
}

void scan_result_free(ScanResult *res) {
    for (size_t i = 0; i < res->file_count; i++) {
        free(res->files[i].source_path);
        free(res->files[i].rel_path);
        free(res->files[i].user_host);
    }
    free(res->files);
    strlist_free(&res->dirs);
    memset(res, 0, sizeof(*res));
}

static void add_file_item(ScanResult *res, const char *src, const char *rel,
                          uint64_t size, bool is_remote, const char *user_host) {
    if (res->file_count == res->file_cap) {
        size_t new_cap = res->file_cap == 0 ? 64 : res->file_cap * 2;
        FileItem *new_files = realloc(res->files, new_cap * sizeof(FileItem));
        if (!new_files) {
            fprintf(stderr, "scpc: out of memory\n");
            exit(1);
        }
        res->files = new_files;
        res->file_cap = new_cap;
    }
    FileItem *item = &res->files[res->file_count++];
    item->source_path = xstrdup_s(src);
    item->rel_path = xstrdup_s(rel);
    item->size = size;
    item->is_remote = is_remote;
    item->user_host = xstrdup_s(user_host);
    res->total_bytes += size;
}

/* Open-addressed hash set for O(1) (dev, ino) cycle detection */
typedef struct {
    dev_t dev;
    ino_t ino;
    bool used;
} InodeSlot;

typedef struct {
    InodeSlot *slots;
    size_t cap;
    size_t count;
} InodeSet;

static uint64_t hash_inode(dev_t dev, ino_t ino) {
    uint64_t h = ((uint64_t)dev * 0x9e3779b97f4a7c15ULL) ^ (uint64_t)ino;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

static void inode_set_init(InodeSet *set) {
    set->cap = 1024;
    set->count = 0;
    set->slots = calloc(set->cap, sizeof(InodeSlot));
    if (!set->slots) {
        fprintf(stderr, "scpc: out of memory\n");
        exit(1);
    }
}

static void inode_set_free(InodeSet *set) {
    free(set->slots);
    set->slots = NULL;
    set->cap = 0;
    set->count = 0;
}

static void inode_set_grow(InodeSet *set) {
    size_t new_cap = set->cap * 2;
    InodeSlot *new_slots = calloc(new_cap, sizeof(InodeSlot));
    if (!new_slots) {
        fprintf(stderr, "scpc: out of memory\n");
        exit(1);
    }
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < set->cap; i++) {
        if (set->slots[i].used) {
            size_t idx = (size_t)hash_inode(set->slots[i].dev, set->slots[i].ino) & mask;
            while (new_slots[idx].used) {
                idx = (idx + 1) & mask;
            }
            new_slots[idx] = set->slots[i];
        }
    }
    free(set->slots);
    set->slots = new_slots;
    set->cap = new_cap;
}

/* Returns true if newly inserted, false if already present. */
static bool inode_set_insert(InodeSet *set, dev_t dev, ino_t ino) {
    if ((set->count + 1) * 10 >= set->cap * 7) {
        inode_set_grow(set);
    }
    size_t mask = set->cap - 1;
    size_t idx = (size_t)hash_inode(dev, ino) & mask;
    while (set->slots[idx].used) {
        if (set->slots[idx].dev == dev && set->slots[idx].ino == ino) {
            return false;
        }
        idx = (idx + 1) & mask;
    }
    set->slots[idx].dev = dev;
    set->slots[idx].ino = ino;
    set->slots[idx].used = true;
    set->count++;
    return true;
}

static const char *path_basename_c(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static char *strip_trailing_slashes(const char *path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    char *out = malloc(len + 1);
    if (!out) {
        fprintf(stderr, "scpc: out of memory\n");
        exit(1);
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}

static char *join_path(const char *a, const char *b) {
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    bool need_slash = alen > 0 && a[alen - 1] != '/';
    char *out = malloc(alen + blen + (need_slash ? 2 : 1));
    if (!out) {
        fprintf(stderr, "scpc: out of memory\n");
        exit(1);
    }
    memcpy(out, a, alen);
    size_t pos = alen;
    if (need_slash) out[pos++] = '/';
    memcpy(out + pos, b, blen);
    out[pos + blen] = '\0';
    return out;
}

/* Parallel directory scanning work queue */
typedef struct {
    char *abs_path;
    char *rel_path;
} DirTask;

typedef struct {
    DirTask *tasks;
    size_t head;
    size_t tail;
    size_t cap;
    size_t active_workers;
    bool done;

    InodeSet visited;
    ScanResult *out;
    ScanProgress *sp;

    pthread_mutex_t queue_lock;
    pthread_cond_t queue_cond;
    pthread_mutex_t out_lock;
} ParallelScanCtx;

static void dir_queue_push_locked(ParallelScanCtx *ctx, char *abs_p, char *rel_p) {
    if (ctx->tail == ctx->cap) {
        size_t count = ctx->tail - ctx->head;
        if (ctx->head > 0 && count < ctx->cap / 2) {
            memmove(ctx->tasks, ctx->tasks + ctx->head, count * sizeof(DirTask));
            ctx->head = 0;
            ctx->tail = count;
        } else {
            size_t new_cap = ctx->cap == 0 ? 64 : ctx->cap * 2;
            DirTask *new_tasks = realloc(ctx->tasks, new_cap * sizeof(DirTask));
            if (!new_tasks) {
                fprintf(stderr, "scpc: out of memory\n");
                exit(1);
            }
            ctx->tasks = new_tasks;
            ctx->cap = new_cap;
        }
    }
    ctx->tasks[ctx->tail++] = (DirTask){.abs_path = abs_p, .rel_path = rel_p};
}

static void *scan_worker_fn(void *arg) {
    ParallelScanCtx *ctx = (ParallelScanCtx *)arg;

    for (;;) {
        pthread_mutex_lock(&ctx->queue_lock);
        while (ctx->head == ctx->tail && !ctx->done) {
            if (ctx->active_workers == 0) {
                ctx->done = true;
                pthread_cond_broadcast(&ctx->queue_cond);
                break;
            }
            pthread_cond_wait(&ctx->queue_cond, &ctx->queue_lock);
        }
        if (ctx->done && ctx->head == ctx->tail) {
            pthread_mutex_unlock(&ctx->queue_lock);
            break;
        }

        DirTask task = ctx->tasks[ctx->head++];
        ctx->active_workers++;
        pthread_mutex_unlock(&ctx->queue_lock);

        /* Thread-local buffers to avoid locking out_lock per file */
        ScanResult local_res;
        scan_result_init(&local_res);
        strlist_push(&local_res.dirs, task.rel_path);

        DirTask *pending_dirs = NULL;
        size_t pending_count = 0;
        size_t pending_cap = 0;

        DIR *dp = opendir(task.abs_path);
        if (dp) {
            struct dirent *ent;
            while ((ent = readdir(dp)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                    continue;
                }
                char *child_abs = join_path(task.abs_path, ent->d_name);
                char *child_rel = (*task.rel_path != '\0')
                                      ? join_path(task.rel_path, ent->d_name)
                                      : xstrdup_s(ent->d_name);

                struct stat cst;
                if (stat(child_abs, &cst) == 0) {
                    if (S_ISDIR(cst.st_mode)) {
                        if (pending_count == pending_cap) {
                            pending_cap = pending_cap == 0 ? 16 : pending_cap * 2;
                            pending_dirs = realloc(pending_dirs, pending_cap * sizeof(DirTask));
                        }
                        pending_dirs[pending_count++] = (DirTask){
                            .abs_path = child_abs,
                            .rel_path = child_rel,
                        };
                        child_abs = NULL;
                        child_rel = NULL;
                    } else if (S_ISREG(cst.st_mode)) {
                        add_file_item(&local_res, child_abs, child_rel,
                                      (uint64_t)cst.st_size, false, NULL);
                    }
                }
                free(child_abs);
                free(child_rel);
            }
            closedir(dp);
        }

        free(task.abs_path);
        free(task.rel_path);

        /* Merge local results into shared ScanResult */
        if (local_res.file_count > 0 || local_res.dirs.count > 0) {
            pthread_mutex_lock(&ctx->out_lock);
            for (size_t i = 0; i < local_res.dirs.count; i++) {
                strlist_push(&ctx->out->dirs, local_res.dirs.items[i]);
            }
            for (size_t i = 0; i < local_res.file_count; i++) {
                const FileItem *fi = &local_res.files[i];
                add_file_item(ctx->out, fi->source_path, fi->rel_path,
                              fi->size, false, NULL);
            }
            pthread_mutex_unlock(&ctx->out_lock);
        }
        scan_progress_update(ctx->sp, local_res.file_count, 1, local_res.total_bytes);
        scan_result_free(&local_res);

        /* Enqueue discovered subdirectories under queue_lock */
        pthread_mutex_lock(&ctx->queue_lock);
        for (size_t i = 0; i < pending_count; i++) {
            struct stat st;
            if (stat(pending_dirs[i].abs_path, &st) == 0 &&
                inode_set_insert(&ctx->visited, st.st_dev, st.st_ino)) {
                dir_queue_push_locked(ctx, pending_dirs[i].abs_path, pending_dirs[i].rel_path);
            } else {
                free(pending_dirs[i].abs_path);
                free(pending_dirs[i].rel_path);
            }
        }
        free(pending_dirs);

        ctx->active_workers--;
        if (ctx->head < ctx->tail) {
            pthread_cond_broadcast(&ctx->queue_cond);
        } else if (ctx->active_workers == 0) {
            ctx->done = true;
            pthread_cond_broadcast(&ctx->queue_cond);
        }
        pthread_mutex_unlock(&ctx->queue_lock);
    }

    return NULL;
}

static int scan_local_sources_mt(const StrList *sources, bool recursive, int num_workers,
                                 bool quiet, ScanResult *out,
                                 char *errbuf, size_t errbuf_sz) {
    scan_result_init(out);

    if (sources->count == 1) {
        struct stat st;
        if (stat(sources->items[0], &st) == 0 && S_ISREG(st.st_mode)) {
            out->single_file_source = true;
        }
    }

    int workers = num_workers < 1 ? 1 : (num_workers > 16 ? 16 : num_workers);
    ScanProgress sp;
    scan_progress_init(&sp, workers, quiet);

    ParallelScanCtx ctx = {0};
    inode_set_init(&ctx.visited);
    ctx.out = out;
    ctx.sp = &sp;
    pthread_mutex_init(&ctx.queue_lock, NULL);
    pthread_cond_init(&ctx.queue_cond, NULL);
    pthread_mutex_init(&ctx.out_lock, NULL);

    for (size_t i = 0; i < sources->count; i++) {
        const char *src = sources->items[i];
        struct stat st;
        if (stat(src, &st) != 0) {
            snprintf(errbuf, errbuf_sz, "%s: %s", src, strerror(errno));
            goto fail;
        }

        char *norm = strip_trailing_slashes(src);
        const char *base = path_basename_c(norm);

        if (S_ISDIR(st.st_mode)) {
            if (!recursive) {
                snprintf(errbuf, errbuf_sz, "%s: not a regular file (use -r for directories)", src);
                free(norm);
                goto fail;
            }
            if (inode_set_insert(&ctx.visited, st.st_dev, st.st_ino)) {
                dir_queue_push_locked(&ctx, norm, xstrdup_s(base));
            } else {
                free(norm);
            }
        } else if (S_ISREG(st.st_mode)) {
            add_file_item(out, src, base, (uint64_t)st.st_size, false, NULL);
            scan_progress_update(&sp, 1, 0, (uint64_t)st.st_size);
            free(norm);
        } else {
            free(norm);
        }
    }

    bool had_dirs = (ctx.head < ctx.tail);
    if (had_dirs) {
        pthread_t *threads = calloc((size_t)workers, sizeof(pthread_t));
        for (int i = 0; i < workers; i++) {
            pthread_create(&threads[i], NULL, scan_worker_fn, &ctx);
        }
        for (int i = 0; i < workers; i++) {
            pthread_join(threads[i], NULL);
        }
        free(threads);
        scan_progress_finish(&sp);
    }

    free(ctx.tasks);
    inode_set_free(&ctx.visited);
    scan_progress_destroy(&sp);
    pthread_mutex_destroy(&ctx.queue_lock);
    pthread_cond_destroy(&ctx.queue_cond);
    pthread_mutex_destroy(&ctx.out_lock);
    return 0;

fail:
    for (size_t j = ctx.head; j < ctx.tail; j++) {
        free(ctx.tasks[j].abs_path);
        free(ctx.tasks[j].rel_path);
    }
    free(ctx.tasks);
    inode_set_free(&ctx.visited);
    scan_progress_destroy(&sp);
    pthread_mutex_destroy(&ctx.queue_lock);
    pthread_cond_destroy(&ctx.queue_cond);
    pthread_mutex_destroy(&ctx.out_lock);
    scan_result_free(out);
    return -1;
}

int scan_local_sources(const StrList *sources, bool recursive, ScanResult *out,
                       char *errbuf, size_t errbuf_sz) {
    int cpus = get_cpu_count();
    int scan_workers = cpus < 2 ? 2 : (cpus > 8 ? 8 : cpus);
    return scan_local_sources_mt(sources, recursive, scan_workers, true, out, errbuf, errbuf_sz);
}

static char *shell_quote(const char *s) {
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

static int scan_remote_sources(const ParsedArgs *args, ScanResult *out,
                               char *errbuf, size_t errbuf_sz) {
    scan_result_init(out);

    /* Parallel remote directory scanner using ThreadPoolExecutor on the remote host */
    static const char *py_probe =
        "import os,sys,threading\n"
        "from concurrent.futures import ThreadPoolExecutor\n"
        "rec=sys.argv[1]=='1'\n"
        "lock=threading.Lock()\n"
        "seen=set()\n"
        "def scan_dir(root_d,rel_d):\n"
        " sub=[];lines=[f'D\\t{rel_d}\\n']\n"
        " try:\n"
        "  with os.scandir(root_d) as it:\n"
        "   for e in it:\n"
        "    try:\n"
        "     st=e.stat(follow_symlinks=True)\n"
        "     if e.is_dir(follow_symlinks=True):\n"
        "      k=(st.st_dev,st.st_ino)\n"
        "      with lock:\n"
        "       if k in seen: continue\n"
        "       seen.add(k)\n"
        "      sub.append((e.path,os.path.join(rel_d,e.name)))\n"
        "     elif e.is_file(follow_symlinks=True):\n"
        "      lines.append(f'F\\t{e.path}\\t{os.path.join(rel_d,e.name)}\\t{st.st_size}\\n')\n"
        "    except OSError: pass\n"
        " except OSError: pass\n"
        " return lines,sub\n"
        "for p in sys.argv[2:]:\n"
        " if not os.path.exists(p): sys.stdout.write(f'E\\t{p}: No such file or directory\\n'); sys.exit(0)\n"
        " np=p.rstrip('/') or p\n"
        " bn=os.path.basename(np)\n"
        " if os.path.isdir(p):\n"
        "  if not rec: sys.stdout.write(f'E\\t{p}: not a regular file\\n'); sys.exit(0)\n"
        "  st=os.stat(np); seen.add((st.st_dev,st.st_ino))\n"
        "  nw=min(8,max(2,os.cpu_count() or 4))\n"
        "  with ThreadPoolExecutor(max_workers=nw) as ex:\n"
        "   futs=[ex.submit(scan_dir,np,bn)]\n"
        "   while futs:\n"
        "    nxt=[]\n"
        "    for f in futs:\n"
        "     lines,sub=f.result()\n"
        "     sys.stdout.writelines(lines)\n"
        "     for sd,sr in sub: nxt.append(ex.submit(scan_dir,sd,sr))\n"
        "    futs=nxt\n"
        " else:\n"
        "  sys.stdout.write(f'F\\t{p}\\t{bn}\\t{os.path.getsize(p)}\\n')\n";

    char *quoted_py = shell_quote(py_probe);
    ScanProgress sp;
    scan_progress_init(&sp, args->workers > 0 ? args->workers : 4, args->quiet || !args->recursive);

    for (size_t i = 0; i < args->sources.count; i++) {
        const char *src = args->sources.items[i];
        if (!is_remote_path(src)) {
            free(quoted_py);
            scan_progress_destroy(&sp);
            scan_result_free(out);
            scan_result_init(out);
            out->known_total = false;
            return 0;
        }

        RemoteSpec spec = {0};
        split_remote_path(src, &spec);
        char *quoted_path = shell_quote(spec.path);

        size_t cmd_len = strlen("python3 -c ") + strlen(quoted_py) + 8 + strlen(quoted_path) + 1;
        char *remote_cmd = malloc(cmd_len);
        if (!remote_cmd) {
            fprintf(stderr, "scpc: out of memory\n");
            exit(1);
        }
        snprintf(remote_cmd, cmd_len, "python3 -c %s %s %s",
                 quoted_py, args->recursive ? "1" : "0", quoted_path);
        free(quoted_path);

        int pipefd[2];
        if (pipe(pipefd) != 0) {
            free(remote_cmd);
            free_remote_spec(&spec);
            free(quoted_py);
            out->known_total = false;
            return 0;
        }

        pid_t pid = fork();
        if (pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }

            size_t ssh_argc = 3 + args->ssh_options.count + 1;
            char **ssh_argv = calloc(ssh_argc, sizeof(char *));
            size_t idx = 0;
            ssh_argv[idx++] = "ssh";
            for (size_t k = 0; k < args->ssh_options.count; k++) {
                ssh_argv[idx++] = args->ssh_options.items[k];
            }
            ssh_argv[idx++] = spec.user_host;
            ssh_argv[idx++] = remote_cmd;
            ssh_argv[idx] = NULL;
            execvp("ssh", ssh_argv);
            _exit(127);
        }

        close(pipefd[1]);
        free(remote_cmd);

        FILE *fp = fdopen(pipefd[0], "r");
        char *line = NULL;
        size_t cap = 0;
        ssize_t nread;
        bool saw_error = false;
        while (fp && (nread = getline(&line, &cap, fp)) != -1) {
            while (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r')) {
                line[--nread] = '\0';
            }
            if (strncmp(line, "E\t", 2) == 0) {
                snprintf(errbuf, errbuf_sz, "%s", line + 2);
                saw_error = true;
                break;
            } else if (strncmp(line, "D\t", 2) == 0) {
                strlist_push(&out->dirs, line + 2);
                scan_progress_update(&sp, 0, 1, 0);
            } else if (strncmp(line, "F\t", 2) == 0) {
                char *p1 = line + 2;
                char *t1 = strchr(p1, '\t');
                char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
                if (t1 && t2) {
                    *t1 = '\0';
                    *t2 = '\0';
                    uint64_t sz = (uint64_t)strtoull(t2 + 1, NULL, 10);
                    add_file_item(out, p1, t1 + 1, sz, true, spec.user_host);
                    scan_progress_update(&sp, 1, 0, sz);
                }
            }
        }
        free(line);
        if (fp) fclose(fp);

        int status = 0;
        waitpid(pid, &status, 0);
        free_remote_spec(&spec);

        if (saw_error) {
            free(quoted_py);
            scan_progress_destroy(&sp);
            scan_result_free(out);
            return -1;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            free(quoted_py);
            scan_progress_destroy(&sp);
            scan_result_free(out);
            scan_result_init(out);
            out->known_total = false;
            return 0;
        }
    }

    if (out->dirs.count > 0) {
        scan_progress_finish(&sp);
    }
    scan_progress_destroy(&sp);
    free(quoted_py);
    if (args->sources.count == 1 && out->file_count == 1 && out->dirs.count == 0) {
        out->single_file_source = true;
    }
    return 0;
}

int scan_sources(const ParsedArgs *args, ScanResult *out,
                 char *errbuf, size_t errbuf_sz) {
    bool any_remote = false;
    bool any_local = false;
    for (size_t i = 0; i < args->sources.count; i++) {
        if (is_remote_path(args->sources.items[i])) {
            any_remote = true;
        } else {
            any_local = true;
        }
    }

    if (any_local && !any_remote) {
        int cpus = get_cpu_count();
        int scan_workers = args->workers > 0
                               ? args->workers
                               : (cpus < 2 ? 2 : (cpus > 8 ? 8 : cpus));
        return scan_local_sources_mt(&args->sources, args->recursive, scan_workers,
                                     args->quiet, out, errbuf, errbuf_sz);
    }
    if (any_remote && !any_local) {
        return scan_remote_sources(args, out, errbuf, errbuf_sz);
    }

    scan_result_init(out);
    out->known_total = false;
    return 0;
}
