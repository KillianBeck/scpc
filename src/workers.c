#include "scpc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

int get_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 4;
}

int determine_workers(size_t file_count, uint64_t total_bytes, int cpu_count) {
    if (file_count <= 1) {
        return 1;
    }
    if (total_bytes < 512ULL * 1024ULL && file_count <= 4) {
        return 1;
    }

    int cpus = cpu_count > 0 ? cpu_count : get_cpu_count();
    double avg_size = (double)total_bytes / (double)file_count;

    int desired;
    if (avg_size < 1024.0 * 1024.0) {
        /* Cap at 6 so we stay well below sshd MaxSessions (10) and avoid thrashing. */
        desired = cpus < 2 ? 2 : (cpus > 6 ? 6 : cpus);
    } else if (avg_size < 10.0 * 1024.0 * 1024.0) {
        int half = cpus / 2;
        if (half < 2) half = 2;
        desired = half > 4 ? 4 : half;
    } else {
        int quarter = cpus / 4;
        if (quarter < 2) quarter = 2;
        desired = quarter > 3 ? 3 : quarter;
    }

    if ((size_t)desired > file_count) {
        desired = (int)file_count;
    }
    return desired < 1 ? 1 : desired;
}

static void process_pty_buffer(char *buf, size_t *len, size_t task_id,
                               int64_t file_size, ProgressTracker *pt) {
    size_t start = 0;
    for (size_t i = 0; i < *len; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') {
            buf[i] = '\0';
            if (pt && i > start) {
                int64_t bytes = parse_scp_progress_line(buf + start, file_size);
                if (bytes >= 0) {
                    progress_update_active(pt, task_id, (uint64_t)bytes);
                }
            }
            start = i + 1;
        }
    }
    if (start > 0) {
        size_t rem = *len - start;
        if (rem > 0) {
            memmove(buf, buf + start, rem);
        }
        *len = rem;
        buf[*len] = '\0';
    }
}

static pthread_mutex_t g_pty_lock = PTHREAD_MUTEX_INITIALIZER;

static int run_scp_with_pipe(char *const argv[], char *err_output, size_t err_output_sz) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        if (err_output && err_output_sz > 0) {
            snprintf(err_output, err_output_sz, "pipe failed: %s", strerror(errno));
        }
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        if (err_output && err_output_sz > 0) {
            snprintf(err_output, err_output_sz, "fork failed: %s", strerror(errno));
        }
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        if (pipefd[1] > STDERR_FILENO) {
            close(pipefd[1]);
        }
        execvp("scp", argv);
        fprintf(stderr, "scpc: failed to exec scp: %s\n", strerror(errno));
        _exit(127);
    }

    close(pipefd[1]);
    size_t err_pos = 0;
    if (err_output && err_output_sz > 0) err_output[0] = '\0';
    char tmp[1024];
    ssize_t n;
    while ((n = read(pipefd[0], tmp, sizeof(tmp))) > 0) {
        if (err_output && err_output_sz > 1 && err_pos + 1 < err_output_sz) {
            size_t copy = (size_t)n;
            if (err_pos + copy >= err_output_sz) {
                copy = err_output_sz - err_pos - 1;
            }
            memcpy(err_output + err_pos, tmp, copy);
            err_pos += copy;
            err_output[err_pos] = '\0';
        }
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

int run_scp_with_pty(char *const argv[], size_t task_id, int64_t file_size,
                     ProgressTracker *pt, bool forward_stdin,
                     char *err_output, size_t err_output_sz) {
    int master_fd = -1, slave_fd = -1;
    int pty_rc = -1;

    pthread_mutex_lock(&g_pty_lock);
    pty_rc = openpty(&master_fd, &slave_fd, NULL, NULL, NULL);
    if (pty_rc == 0) {
        struct termios tio;
        if (tcgetattr(slave_fd, &tio) == 0) {
            tio.c_lflag &= ~(tcflag_t)ECHO;
            tcsetattr(slave_fd, TCSANOW, &tio);
        }
    }
    pthread_mutex_unlock(&g_pty_lock);

    if (pty_rc != 0) {
        /* Fallback to pipe-based execution if PTYs are temporarily exhausted. */
        return run_scp_with_pipe(argv, err_output, err_output_sz);
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(master_fd);
        close(slave_fd);
        if (err_output && err_output_sz > 0) {
            snprintf(err_output, err_output_sz, "fork failed: %s", strerror(errno));
        }
        return -1;
    }

    if (pid == 0) {
        close(master_fd);
        setsid();
#ifdef TIOCSCTTY
        ioctl(slave_fd, TIOCSCTTY, 0);
#endif
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) {
            close(slave_fd);
        }
        execvp("scp", argv);
        fprintf(stderr, "scpc: failed to exec scp: %s\n", strerror(errno));
        _exit(127);
    }

    close(slave_fd);

    char line_buf[4096];
    size_t line_len = 0;
    size_t err_pos = 0;
    if (err_output && err_output_sz > 0) err_output[0] = '\0';

    bool use_stdin = forward_stdin && isatty(STDIN_FILENO);

    for (;;) {
        struct pollfd fds[2];
        nfds_t nfds = 1;
        fds[0].fd = master_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        if (use_stdin) {
            fds[1].fd = STDIN_FILENO;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            nfds = 2;
        }

        int pr = poll(fds, nfds, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char tmp[2048];
            ssize_t n = read(master_fd, tmp, sizeof(tmp));
            if (n <= 0) {
                break;
            }
            if (err_output && err_output_sz > 1 && err_pos + 1 < err_output_sz) {
                size_t copy = (size_t)n;
                if (err_pos + copy >= err_output_sz) {
                    copy = err_output_sz - err_pos - 1;
                }
                memcpy(err_output + err_pos, tmp, copy);
                err_pos += copy;
                err_output[err_pos] = '\0';
            }

            size_t off = 0;
            while (off < (size_t)n) {
                size_t space = sizeof(line_buf) - 1 - line_len;
                size_t chunk = (size_t)n - off;
                if (chunk > space) chunk = space;
                memcpy(line_buf + line_len, tmp + off, chunk);
                line_len += chunk;
                line_buf[line_len] = '\0';
                process_pty_buffer(line_buf, &line_len, task_id, file_size, pt);
                if (line_len == sizeof(line_buf) - 1) {
                    line_len = 0;
                }
                off += chunk;
            }
        }

        if (use_stdin && (fds[1].revents & POLLIN)) {
            char inbuf[512];
            ssize_t n = read(STDIN_FILENO, inbuf, sizeof(inbuf));
            if (n > 0) {
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(master_fd, inbuf + written, (size_t)(n - written));
                    if (w <= 0) break;
                    written += w;
                }
            } else {
                use_stdin = false;
            }
        }
    }

    if (pt && line_len > 0) {
        int64_t bytes = parse_scp_progress_line(line_buf, file_size);
        if (bytes >= 0) {
            progress_update_active(pt, task_id, (uint64_t)bytes);
        }
    }

    close(master_fd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return 1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

