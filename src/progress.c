#include "scpc.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

double now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void format_bytes(uint64_t bytes, char *buf, size_t buf_sz) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double val = (double)bytes;
    size_t u = 0;
    while (val >= 1024.0 && u < 5) {
        val /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(buf, buf_sz, "%llu B", (unsigned long long)bytes);
    } else {
        snprintf(buf, buf_sz, "%.1f %s", val, units[u]);
    }
}

void format_speed(double bytes_per_sec, char *buf, size_t buf_sz) {
    if (bytes_per_sec < 0.0) bytes_per_sec = 0.0;
    char bbuf[32];
    format_bytes((uint64_t)bytes_per_sec, bbuf, sizeof(bbuf));
    snprintf(buf, buf_sz, "%s/s", bbuf);
}

void format_eta(double seconds, char *buf, size_t buf_sz) {
    if (seconds < 0.0 || !isfinite(seconds)) {
        snprintf(buf, buf_sz, "--:--");
        return;
    }
    long total_sec = (long)llround(seconds);
    long hours = total_sec / 3600;
    long rem = total_sec % 3600;
    long mins = rem / 60;
    long secs = rem % 60;
    if (hours > 0) {
        snprintf(buf, buf_sz, "%ldh %02ldm %02lds", hours, mins, secs);
    } else {
        snprintf(buf, buf_sz, "%02ld:%02ld", mins, secs);
    }
}

static int get_terminal_cols(void) {
    struct winsize ws;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return (int)ws.ws_col;
    }
    return 80;
}

void progress_init(ProgressTracker *pt, uint64_t total_bytes, size_t total_files,
                   int workers, bool known_total, bool quiet, size_t num_tasks) {
    memset(pt, 0, sizeof(*pt));
    pt->total_bytes = total_bytes;
    pt->total_files = total_files;
    pt->completed_files = 0;
    pt->workers = workers < 1 ? 1 : workers;
    pt->known_total = known_total;
    pt->quiet = quiet;
    pt->num_tasks = num_tasks;
    pt->active_bytes = num_tasks > 0 ? calloc(num_tasks, sizeof(uint64_t)) : NULL;
    pt->start_time = now_monotonic();
    pthread_mutex_init(&pt->lock, NULL);
}

void progress_destroy(ProgressTracker *pt) {
    free(pt->active_bytes);
    pt->active_bytes = NULL;
    pthread_mutex_destroy(&pt->lock);
}

static uint64_t current_bytes_locked(const ProgressTracker *pt) {
    uint64_t total = pt->completed_bytes;
    for (size_t i = 0; i < pt->num_tasks; i++) {
        total += pt->active_bytes[i];
    }
    if (pt->known_total && pt->total_bytes > 0 && total > pt->total_bytes) {
        return pt->total_bytes;
    }
    return total;
}

static void build_line_locked(const ProgressTracker *pt, double now,
                              char *buf, size_t buf_sz) {
    double elapsed = now - pt->start_time;
    if (elapsed < 1e-6) elapsed = 1e-6;

    uint64_t transferred = current_bytes_locked(pt);
    double avg_speed = (double)transferred / elapsed;

    char done_str[32], total_str[32], spd_str[32], eta_str[32];
    format_bytes(transferred, done_str, sizeof(done_str));
    format_speed(avg_speed, spd_str, sizeof(spd_str));
    const char *wlabel = pt->workers == 1 ? "worker" : "workers";

    if (pt->known_total && pt->total_bytes > 0) {
        double ratio = (double)transferred / (double)pt->total_bytes;
        if (ratio > 1.0) ratio = 1.0;
        double pct = ratio * 100.0;
        uint64_t remaining = pt->total_bytes > transferred ? (pt->total_bytes - transferred) : 0;
        if (remaining == 0) {
            snprintf(eta_str, sizeof(eta_str), "00:00");
        } else if (avg_speed > 0.0) {
            format_eta((double)remaining / avg_speed, eta_str, sizeof(eta_str));
        } else {
            snprintf(eta_str, sizeof(eta_str), "--:--");
        }

        format_bytes(pt->total_bytes, total_str, sizeof(total_str));
        char stats[256];
        if (pt->total_files > 0) {
            snprintf(stats, sizeof(stats),
                     "%5.1f%%  %s / %s | Transferred: %zu/%zu Files | %s | ETA %s | %d %s",
                     pct, done_str, total_str, pt->completed_files, pt->total_files,
                     spd_str, eta_str, pt->workers, wlabel);
        } else {
            snprintf(stats, sizeof(stats),
                     "%5.1f%%  %s / %s | %s | ETA %s | %d %s",
                     pct, done_str, total_str, spd_str, eta_str, pt->workers, wlabel);
        }

        int cols = get_terminal_cols();
        int bar_w = cols - (int)strlen(stats) - 4;
        if (bar_w < 10) bar_w = 10;
        if (bar_w > 32) bar_w = 32;
        int filled = (int)llround(ratio * bar_w);
        if (filled > bar_w) filled = bar_w;

        char bar[160];
        size_t pos = 0;
        bar[pos++] = '[';
        for (int i = 0; i < bar_w && pos + 4 < sizeof(bar); i++) {
            if (i < filled) {
                memcpy(bar + pos, "\xe2\x96\x88", 3); /* █ */
                pos += 3;
            } else {
                memcpy(bar + pos, "\xe2\x96\x91", 3); /* ░ */
                pos += 3;
            }
        }
        bar[pos++] = ']';
        bar[pos] = '\0';

        snprintf(buf, buf_sz, "%s %s", bar, stats);
    } else if (pt->known_total && pt->total_bytes == 0) {
        snprintf(buf, buf_sz,
                 "[████████████████████] 100.0%%  0 B / 0 B | Transferred: %zu/%zu Files | 0 B/s | ETA 00:00 | %d %s",
                 pt->completed_files, pt->total_files, pt->workers, wlabel);
    } else {
        snprintf(buf, buf_sz,
                 "Transferred %s | %s | ETA --:-- | %d %s",
                 done_str, spd_str, pt->workers, wlabel);
    }
}

static void render_locked(ProgressTracker *pt, bool force) {
    if (pt->quiet) return;
    double now = now_monotonic();
    if (!force && (now - pt->last_render_time) < 0.1) {
        return;
    }
    pt->last_render_time = now;

    char line[512];
    build_line_locked(pt, now, line, sizeof(line));
    if (isatty(STDERR_FILENO)) {
        fprintf(stderr, "\r%s\x1b[K", line);
    } else if (force || pt->finished) {
        fprintf(stderr, "%s\n", line);
    }
    fflush(stderr);
}

void progress_start(ProgressTracker *pt) {
    pthread_mutex_lock(&pt->lock);
    pt->start_time = now_monotonic();
    render_locked(pt, true);
    pthread_mutex_unlock(&pt->lock);
}

void progress_update_active(ProgressTracker *pt, size_t task_id, uint64_t bytes_done) {
    pthread_mutex_lock(&pt->lock);
    if (task_id < pt->num_tasks && bytes_done > pt->active_bytes[task_id]) {
        pt->active_bytes[task_id] = bytes_done;
    }
    render_locked(pt, false);
    pthread_mutex_unlock(&pt->lock);
}

void progress_complete_task(ProgressTracker *pt, size_t task_id,
                            uint64_t task_bytes, size_t task_files) {
    pthread_mutex_lock(&pt->lock);
    if (task_id < pt->num_tasks) {
        pt->active_bytes[task_id] = 0;
    }
    pt->completed_bytes += task_bytes;
    pt->completed_files += task_files;
    if (pt->total_files > 0 && pt->completed_files > pt->total_files) {
        pt->completed_files = pt->total_files;
    }
    render_locked(pt, false);
    pthread_mutex_unlock(&pt->lock);
}

void progress_finish(ProgressTracker *pt, bool success) {
    pthread_mutex_lock(&pt->lock);
    if (pt->finished) {
        pthread_mutex_unlock(&pt->lock);
        return;
    }
    pt->finished = true;
    if (success && pt->known_total) {
        for (size_t i = 0; i < pt->num_tasks; i++) {
            pt->active_bytes[i] = 0;
        }
        if (pt->total_bytes > 0) {
            pt->completed_bytes = pt->total_bytes;
        }
        if (pt->total_files > 0) {
            pt->completed_files = pt->total_files;
        }
    }
    render_locked(pt, true);
    if (!pt->quiet) {
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    pthread_mutex_unlock(&pt->lock);
}

void progress_format_line(ProgressTracker *pt, double now, char *buf, size_t buf_sz) {
    pthread_mutex_lock(&pt->lock);
    build_line_locked(pt, now, buf, buf_sz);
    pthread_mutex_unlock(&pt->lock);
}

int64_t parse_scp_progress_line(const char *line, int64_t file_size) {
    if (!line) return -1;

    /* Look for "<pct>% <amount><unit>" */
    const char *p = line;
    int64_t best = -1;

    while (*p) {
        if (isdigit((unsigned char)*p)) {
            char *end_pct = NULL;
            long pct = strtol(p, &end_pct, 10);
            if (end_pct > p && *end_pct == '%' && pct >= 0 && pct <= 100) {
                const char *q = end_pct + 1;
                while (*q == ' ' || *q == '\t') q++;
                if (isdigit((unsigned char)*q)) {
                    char *end_amt = NULL;
                    double amt = strtod(q, &end_amt);
                    if (end_amt > q) {
                        while (*end_amt == ' ') end_amt++;
                        uint64_t mult = 0;
                        if (strncasecmp(end_amt, "PB", 2) == 0) mult = 1024ULL * 1024 * 1024 * 1024 * 1024;
                        else if (strncasecmp(end_amt, "TB", 2) == 0) mult = 1024ULL * 1024 * 1024 * 1024;
                        else if (strncasecmp(end_amt, "GB", 2) == 0) mult = 1024ULL * 1024 * 1024;
                        else if (strncasecmp(end_amt, "MB", 2) == 0) mult = 1024ULL * 1024;
                        else if (strncasecmp(end_amt, "KB", 2) == 0) mult = 1024ULL;
                        else if (toupper((unsigned char)*end_amt) == 'B') mult = 1ULL;

                        if (mult > 0) {
                            int64_t parsed = (int64_t)(amt * (double)mult);
                            if (file_size >= 0) {
                                if (pct >= 100) parsed = file_size;
                                else if (parsed > file_size) parsed = file_size;
                            }
                            best = parsed;
                        }
                    }
                }
            }
        }
        p++;
    }
    return best;
}

void scan_progress_init(ScanProgress *sp, int workers, bool quiet) {
    memset(sp, 0, sizeof(*sp));
    sp->workers = workers < 1 ? 1 : workers;
    sp->quiet = quiet;
    sp->start_time = now_monotonic();
    pthread_mutex_init(&sp->lock, NULL);
}

void scan_progress_destroy(ScanProgress *sp) {
    pthread_mutex_destroy(&sp->lock);
}

static void build_scan_line_locked(const ScanProgress *sp, double now,
                                   char *buf, size_t buf_sz) {
    char size_str[32];
    format_bytes(sp->bytes_found, size_str, sizeof(size_str));
    const char *wlabel = sp->workers == 1 ? "worker" : "workers";

    const int bar_w = 20;
    const int block_w = 5;
    char bar[128];
    size_t pos = 0;
    bar[pos++] = '[';

    if (sp->finished) {
        for (int i = 0; i < bar_w; i++) {
            memcpy(bar + pos, "\xe2\x96\x88", 3); /* █ */
            pos += 3;
        }
        bar[pos++] = ']';
        bar[pos] = '\0';
        double elapsed = now - sp->start_time;
        if (elapsed < 0.0) elapsed = 0.0;
        snprintf(buf, buf_sz,
                 "Scanning %s %zu files | %zu dirs | %s in %.2fs | %d %s",
                 bar, sp->files_scanned, sp->dirs_scanned, size_str,
                 elapsed, sp->workers, wlabel);
    } else {
        int span = bar_w - block_w;
        int cycle = span * 2;
        int step = (int)(sp->tick % (size_t)(cycle > 0 ? cycle : 1));
        int start = step <= span ? step : (cycle - step);
        for (int i = 0; i < bar_w; i++) {
            if (i >= start && i < start + block_w) {
                memcpy(bar + pos, "\xe2\x96\x88", 3); /* █ */
            } else {
                memcpy(bar + pos, "\xe2\x96\x91", 3); /* ░ */
            }
            pos += 3;
        }
        bar[pos++] = ']';
        bar[pos] = '\0';
        snprintf(buf, buf_sz,
                 "Scanning %s %zu files | %zu dirs | %s | %d %s",
                 bar, sp->files_scanned, sp->dirs_scanned, size_str,
                 sp->workers, wlabel);
    }
}

static void scan_render_locked(ScanProgress *sp, bool force) {
    if (sp->quiet) return;
    double now = now_monotonic();
    if (!force && (now - sp->last_render_time) < 0.08) {
        return;
    }
    sp->last_render_time = now;
    sp->tick++;

    char line[512];
    build_scan_line_locked(sp, now, line, sizeof(line));
    if (isatty(STDERR_FILENO)) {
        fprintf(stderr, "\r%s\x1b[K", line);
    } else if (force) {
        fprintf(stderr, "%s\n", line);
    }
    fflush(stderr);
}

void scan_progress_update(ScanProgress *sp, size_t add_files, size_t add_dirs,
                          uint64_t add_bytes) {
    if (!sp) return;
    pthread_mutex_lock(&sp->lock);
    sp->files_scanned += add_files;
    sp->dirs_scanned += add_dirs;
    sp->bytes_found += add_bytes;
    scan_render_locked(sp, false);
    pthread_mutex_unlock(&sp->lock);
}

void scan_progress_finish(ScanProgress *sp) {
    if (!sp) return;
    pthread_mutex_lock(&sp->lock);
    if (sp->finished) {
        pthread_mutex_unlock(&sp->lock);
        return;
    }
    sp->finished = true;
    scan_render_locked(sp, true);
    if (!sp->quiet) {
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    pthread_mutex_unlock(&sp->lock);
}

void scan_progress_format_line(ScanProgress *sp, double now, char *buf, size_t buf_sz) {
    pthread_mutex_lock(&sp->lock);
    build_scan_line_locked(sp, now, buf, buf_sz);
    pthread_mutex_unlock(&sp->lock);
}

