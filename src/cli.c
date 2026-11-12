#include "scpc.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *xstrdup(const char *s) {
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

void strlist_init(StrList *list) {
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

void strlist_push(StrList *list, const char *s) {
    if (list->count == list->cap) {
        size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
        char **new_items = realloc(list->items, new_cap * sizeof(char *));
        if (!new_items) {
            fprintf(stderr, "scpc: out of memory\n");
            exit(1);
        }
        list->items = new_items;
        list->cap = new_cap;
    }
    list->items[list->count++] = xstrdup(s);
}

void strlist_free(StrList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

void parsed_args_init(ParsedArgs *args) {
    memset(args, 0, sizeof(*args));
    strlist_init(&args->scp_options);
    strlist_init(&args->ssh_options);
    strlist_init(&args->sources);
}

void parsed_args_free(ParsedArgs *args) {
    free(args->port);
    free(args->target);
    strlist_free(&args->scp_options);
    strlist_free(&args->ssh_options);
    strlist_free(&args->sources);
    memset(args, 0, sizeof(*args));
}

void print_help(void) {
    printf(
        "usage: scpc [-w workers] [-346ABCOpqRrsTv] [-c cipher] [-D sftp_server_path]\n"
        "            [-F ssh_config] [-i identity_file] [-J destination] [-l limit]\n"
        "            [-o ssh_option] [-P port] [-S program] [-X sftp_option]\n"
        "            source ... target\n\n"
        "C wrapper around scp with progress bar, average transfer speed, ETA,\n"
        "recursive directory sizing, and parallel worker transfers.\n\n"
        "scpc options:\n"
        "  -w, --workers N   Number of parallel transfer workers (default: auto)\n"
        "  -o asc|desc       Sort files by size ascending or descending\n"
        "                    (other -o Key=Value options are forwarded to ssh/scp)\n"
        "  -s, --skip        Skip existing destination files without prompting\n"
        "  -f, --full        Overwrite all existing destination files without prompting\n"
        "  -h, --help        Show this help message and exit\n\n"
        "All standard scp options (including -r for recursive directory transfer)\n"
        "are supported and forwarded to scp.\n"
    );
}

bool is_remote_path(const char *path) {
    if (!path || path[0] == '\0') return false;
    if (path[0] == '/' ||
        (path[0] == '.' && path[1] == '/') ||
        (path[0] == '.' && path[1] == '.' && path[2] == '/') ||
        (path[0] == '~' && path[1] == '/')) {
        return false;
    }

    const char *p = path;
    /* Optional user@ */
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');

    if (at && (!slash || at < slash)) {
        if (at == p) return false; /* empty user */
        p = at + 1;
    }

    if (*p == '[') {
        const char *rbracket = strchr(p, ']');
        if (!rbracket || rbracket == p + 1 || rbracket[1] != ':') {
            return false;
        }
        if (slash && slash < rbracket + 1) return false;
        return true;
    }

    const char *colon = strchr(p, ':');
    if (!colon || colon == p) return false;
    if (slash && slash < colon) return false;
    return true;
}

bool split_remote_path(const char *path, RemoteSpec *out) {
    if (!is_remote_path(path)) return false;
    const char *p = path;
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');
    if (at && (!slash || at < slash)) {
        p = at + 1;
    }
    const char *colon = (*p == '[') ? (strchr(p, ']') + 1) : strchr(p, ':');
    size_t host_len = (size_t)(colon - path);
    out->user_host = malloc(host_len + 1);
    if (!out->user_host) {
        fprintf(stderr, "scpc: out of memory\n");
        exit(1);
    }
    memcpy(out->user_host, path, host_len);
    out->user_host[host_len] = '\0';

    const char *rpath = colon + 1;
    out->path = xstrdup(*rpath ? rpath : ".");
    return true;
}

void free_remote_spec(RemoteSpec *spec) {
    free(spec->user_host);
    free(spec->path);
    spec->user_host = NULL;
    spec->path = NULL;
}

static bool is_scp_bool_flag(char ch) {
    return strchr("346ABCOpqRrTv", ch) != NULL;
}

static bool is_scp_value_flag(char ch) {
    return strchr("cDFiJloPSX", ch) != NULL;
}

static bool str_contains_ci(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

static int parse_positive_int(const char *s, const char *opt_name,
                              char *errbuf, size_t errbuf_sz) {
    if (!s || !*s) {
        snprintf(errbuf, errbuf_sz, "Option %s requires a positive integer", opt_name);
        return -1;
    }
    char *end = NULL;
    errno = 0;
    long val = strtol(s, &end, 10);
    if (errno != 0 || !end || *end != '\0' || val < 1 || val > 1024) {
        snprintf(errbuf, errbuf_sz, "Invalid value for %s: %s", opt_name, s);
        return -1;
    }
    return (int)val;
}

static int record_scp_flag(ParsedArgs *args, char ch, const char *val,
                           char *errbuf, size_t errbuf_sz) {
    if (ch == 'o' && val != NULL) {
        if (strcasecmp(val, "asc") == 0) {
            args->sort_order = SORT_ASC;
            return 0;
        }
        if (strcasecmp(val, "desc") == 0) {
            args->sort_order = SORT_DESC;
            return 0;
        }
    }

    char opt[3] = {'-', ch, '\0'};
    strlist_push(&args->scp_options, opt);

    if (!val) {
        if (ch == 'r') args->recursive = true;
        else if (ch == 'q') {
            args->quiet = true;
            strlist_push(&args->ssh_options, opt);
        } else if (ch == 'v') {
            args->verbose = true;
            strlist_push(&args->ssh_options, opt);
        } else if (ch == 'B') {
            args->batch_mode = true;
            strlist_push(&args->ssh_options, "-o");
            strlist_push(&args->ssh_options, "BatchMode=yes");
        } else if (ch == 'p') {
            args->preserve = true;
        } else if (ch == '4' || ch == '6' || ch == 'A' || ch == 'C') {
            strlist_push(&args->ssh_options, opt);
        }
        return 0;
    }

    strlist_push(&args->scp_options, val);
    if (ch == 'P') {
        free(args->port);
        args->port = xstrdup(val);
        strlist_push(&args->ssh_options, "-p");
        strlist_push(&args->ssh_options, val);
    } else if (ch == 'l') {
        char *end = NULL;
        errno = 0;
        long kbps = strtol(val, &end, 10);
        if (errno != 0 || !end || *end != '\0' || kbps <= 0) {
            snprintf(errbuf, errbuf_sz, "Invalid bandwidth limit for -l: %s", val);
            return -1;
        }
        args->limit_kbps = kbps;
    } else if (ch == 'c' || ch == 'F' || ch == 'i' || ch == 'J' || ch == 'o' || ch == 'S') {
        strlist_push(&args->ssh_options, opt);
        strlist_push(&args->ssh_options, val);
        if (ch == 'o' && str_contains_ci(val, "controlpath")) {
            args->has_custom_control_path = true;
        }
        if (ch == 'S') {
            args->has_custom_program = true;
        }
    } else if (ch == 'D') {
        args->has_custom_program = true;
    }

    return 0;
}

int parse_args(int argc, char **argv, ParsedArgs *out, char *errbuf, size_t errbuf_sz) {
    parsed_args_init(out);
    StrList positional;
    strlist_init(&positional);

    bool end_of_opts = false;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (end_of_opts) {
            strlist_push(&positional, arg);
            continue;
        }

        if (strcmp(arg, "--") == 0) {
            end_of_opts = true;
            continue;
        }

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            out->show_help = true;
            strlist_free(&positional);
            return 0;
        }

        if (strcmp(arg, "--skip") == 0) {
            out->overwrite_mode = OVERWRITE_SKIP;
            continue;
        }

        if (strcmp(arg, "--full") == 0) {
            out->overwrite_mode = OVERWRITE_FULL;
            continue;
        }

        if (strcmp(arg, "--workers") == 0) {
            if (i + 1 >= argc) {
                snprintf(errbuf, errbuf_sz, "Option --workers requires an argument");
                goto fail;
            }
            int w = parse_positive_int(argv[++i], "--workers", errbuf, errbuf_sz);
            if (w < 0) goto fail;
            out->workers = w;
            continue;
        }

        if (strncmp(arg, "--workers=", 10) == 0) {
            int w = parse_positive_int(arg + 10, "--workers", errbuf, errbuf_sz);
            if (w < 0) goto fail;
            out->workers = w;
            continue;
        }

        if (strncmp(arg, "--", 2) == 0) {
            snprintf(errbuf, errbuf_sz, "Unknown option: %s", arg);
            goto fail;
        }

        if (arg[0] == '-' && arg[1] != '\0') {
            for (size_t j = 1; arg[j] != '\0'; j++) {
                char ch = arg[j];
                if (ch == 'h') {
                    out->show_help = true;
                    strlist_free(&positional);
                    return 0;
                }
                if (ch == 's') {
                    out->overwrite_mode = OVERWRITE_SKIP;
                    continue;
                }
                if (ch == 'f') {
                    out->overwrite_mode = OVERWRITE_FULL;
                    continue;
                }
                if (ch == 'w') {
                    const char *rest = &arg[j + 1];
                    const char *val = NULL;
                    if (*rest != '\0') {
                        val = rest;
                    } else if (i + 1 < argc) {
                        val = argv[++i];
                    } else {
                        snprintf(errbuf, errbuf_sz, "Option -w requires an argument");
                        goto fail;
                    }
                    int w = parse_positive_int(val, "-w", errbuf, errbuf_sz);
                    if (w < 0) goto fail;
                    out->workers = w;
                    break;
                }
                if (is_scp_bool_flag(ch)) {
                    if (record_scp_flag(out, ch, NULL, errbuf, errbuf_sz) != 0) goto fail;
                } else if (is_scp_value_flag(ch)) {
                    const char *rest = &arg[j + 1];
                    const char *val = NULL;
                    if (*rest != '\0') {
                        val = rest;
                    } else if (i + 1 < argc) {
                        val = argv[++i];
                    } else {
                        snprintf(errbuf, errbuf_sz, "Option -%c requires an argument", ch);
                        goto fail;
                    }
                    if (record_scp_flag(out, ch, val, errbuf, errbuf_sz) != 0) goto fail;
                    break;
                } else {
                    snprintf(errbuf, errbuf_sz, "Illegal option -- %c", ch);
                    goto fail;
                }
            }
            continue;
        }

        strlist_push(&positional, arg);
    }

    if (positional.count < 2) {
        snprintf(errbuf, errbuf_sz, "Both source and target must be specified");
        goto fail;
    }

    for (size_t i = 0; i < positional.count - 1; i++) {
        strlist_push(&out->sources, positional.items[i]);
    }
    out->target = xstrdup(positional.items[positional.count - 1]);
    strlist_free(&positional);
    return 0;

fail:
    strlist_free(&positional);
    parsed_args_free(out);
    return -1;
}
