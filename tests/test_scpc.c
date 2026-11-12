#include "../src/scpc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_cli_parsing(void) {
    char err[256] = {0};

    /* Basic -r and -w */
    char *argv1[] = {"scpc", "-r", "-w", "4", "-P", "2222", "-p", "-C", "src_dir", "user@host:/dest"};
    ParsedArgs args1;
    assert(parse_args(10, argv1, &args1, err, sizeof(err)) == 0);
    assert(args1.recursive == true);
    assert(args1.workers == 4);
    assert(strcmp(args1.port, "2222") == 0);
    assert(args1.preserve == true);
    assert(args1.sources.count == 1);
    assert(strcmp(args1.sources.items[0], "src_dir") == 0);
    assert(strcmp(args1.target, "user@host:/dest") == 0);
    parsed_args_free(&args1);

    /* Combined short flags -rqw6 and --workers=8 */
    char *argv2[] = {"scpc", "-rqw6", "a.txt", "b.txt", "host:dir/"};
    ParsedArgs args2;
    assert(parse_args(5, argv2, &args2, err, sizeof(err)) == 0);
    assert(args2.recursive == true);
    assert(args2.quiet == true);
    assert(args2.workers == 6);
    assert(args2.sources.count == 2);
    parsed_args_free(&args2);

    /* Auto workers when -w omitted */
    char *argv3[] = {"scpc", "--workers=3", "file", "host:/tmp"};
    ParsedArgs args3;
    assert(parse_args(4, argv3, &args3, err, sizeof(err)) == 0);
    assert(args3.workers == 3);
    parsed_args_free(&args3);

    /* Invalid -w 0 */
    char *argv4[] = {"scpc", "-w", "0", "file", "host:/tmp"};
    ParsedArgs args4;
    assert(parse_args(5, argv4, &args4, err, sizeof(err)) != 0);

    /* Help flag */
    char *argv5[] = {"scpc", "--help"};
    ParsedArgs args5;
    assert(parse_args(2, argv5, &args5, err, sizeof(err)) == 0);
    assert(args5.show_help == true);
    parsed_args_free(&args5);

    /* -o desc / -o asc size ordering vs standard -o ssh_option */
    char *argv6[] = {"scpc", "-r", "-o", "desc", "-o", "StrictHostKeyChecking=no", "src", "host:dst"};
    ParsedArgs args6;
    assert(parse_args(8, argv6, &args6, err, sizeof(err)) == 0);
    assert(args6.sort_order == SORT_DESC);
    assert(args6.scp_options.count == 3); /* -r, -o, StrictHostKeyChecking=no */
    assert(args6.ssh_options.count == 2); /* -o, StrictHostKeyChecking=no */
    parsed_args_free(&args6);

    char *argv7[] = {"scpc", "-oasc", "src", "host:dst"};
    ParsedArgs args7;
    assert(parse_args(4, argv7, &args7, err, sizeof(err)) == 0);
    assert(args7.sort_order == SORT_ASC);
    assert(args7.scp_options.count == 0);
    parsed_args_free(&args7);

    /* -s / --skip and -f / --full */
    char *argv8[] = {"scpc", "-rs", "src", "host:dst"};
    ParsedArgs args8;
    assert(parse_args(4, argv8, &args8, err, sizeof(err)) == 0);
    assert(args8.recursive == true);
    assert(args8.overwrite_mode == OVERWRITE_SKIP);
    parsed_args_free(&args8);

    char *argv9[] = {"scpc", "--full", "src", "host:dst"};
    ParsedArgs args9;
    assert(parse_args(4, argv9, &args9, err, sizeof(err)) == 0);
    assert(args9.overwrite_mode == OVERWRITE_FULL);
    parsed_args_free(&args9);
}

static void test_remote_path_detection(void) {
    assert(is_remote_path("user@host:/path/to/file") == true);
    assert(is_remote_path("host:file.txt") == true);
    assert(is_remote_path("[::1]:/tmp") == true);
    assert(is_remote_path("/local/path:with:colon") == false);
    assert(is_remote_path("./local:file") == false);
    assert(is_remote_path("../local:file") == false);
    assert(is_remote_path("~/local:file") == false);
    assert(is_remote_path("plain_file.txt") == false);

    RemoteSpec spec = {0};
    assert(split_remote_path("alice@example.com:/var/data", &spec) == true);
    assert(strcmp(spec.user_host, "alice@example.com") == 0);
    assert(strcmp(spec.path, "/var/data") == 0);
    free_remote_spec(&spec);
}

static void write_dummy_file(const char *path, size_t size) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    for (size_t i = 0; i < size; i++) {
        fputc('A' + (int)(i % 26), f);
    }
    fclose(f);
}

static void test_local_recursive_scan(void) {
    char tmpl[] = "/tmp/scpc_test_XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root != NULL);

    char sub1[512], sub2[512], f1[512], f2[512], f3[512];
    snprintf(sub1, sizeof(sub1), "%s/sub1", root);
    snprintf(sub2, sizeof(sub2), "%s/sub1/nested", root);
    assert(mkdir(sub1, 0755) == 0);
    assert(mkdir(sub2, 0755) == 0);

    snprintf(f1, sizeof(f1), "%s/a.bin", root);
    snprintf(f2, sizeof(f2), "%s/b.bin", sub1);
    snprintf(f3, sizeof(f3), "%s/c.bin", sub2);
    write_dummy_file(f1, 100);
    write_dummy_file(f2, 250);
    write_dummy_file(f3, 500);

    StrList srcs;
    strlist_init(&srcs);
    strlist_push(&srcs, root);

    /* Without -r, scanning a directory should fail */
    ScanResult res_no_rec;
    char err[256] = {0};
    assert(scan_local_sources(&srcs, false, &res_no_rec, err, sizeof(err)) != 0);

    /* With -r, scanning should find all 3 files and 850 total bytes */
    ScanResult res;
    assert(scan_local_sources(&srcs, true, &res, err, sizeof(err)) == 0);
    assert(res.file_count == 3);
    assert(res.total_bytes == 850);
    assert(res.dirs.count == 3);

    scan_result_free(&res);
    strlist_free(&srcs);

    unlink(f3);
    unlink(f2);
    unlink(f1);
    rmdir(sub2);
    rmdir(sub1);
    rmdir(root);
}

static void test_determine_workers(void) {
    assert(determine_workers(1, 100ULL * 1024 * 1024, 8) == 1);
    assert(determine_workers(3, 100ULL * 1024, 8) == 1);
    assert(determine_workers(20, 5ULL * 1024 * 1024, 16) == 6);
    assert(determine_workers(10, 500ULL * 1024 * 1024, 16) == 3);
    assert(determine_workers(2, 50ULL * 1024 * 1024, 8) == 2);
}

static void test_progress_and_pty_parser(void) {
    assert(parse_scp_progress_line("file.bin   45%   90MB  18.2MB/s   00:05 ETA", 200LL * 1024 * 1024) ==
           90LL * 1024 * 1024);
    assert(parse_scp_progress_line("file.bin  100%  200MB  18.2MB/s   00:00", 200LL * 1024 * 1024) ==
           200LL * 1024 * 1024);
    assert(parse_scp_progress_line("no progress here", 1000) == -1);

    char bbuf[32], ebuf[32];
    format_bytes(1536, bbuf, sizeof(bbuf));
    assert(strcmp(bbuf, "1.5 KB") == 0);
    format_eta(125.0, ebuf, sizeof(ebuf));
    assert(strcmp(ebuf, "02:05") == 0);
    format_eta(3661.0, ebuf, sizeof(ebuf));
    assert(strcmp(ebuf, "1h 01m 01s") == 0);

    ProgressTracker pt;
    progress_init(&pt, 1000, 4, 4, true, true, 2);
    pt.start_time = now_monotonic() - 2.0;
    progress_update_active(&pt, 0, 250);
    progress_complete_task(&pt, 1, 250, 1);
    char line[512];
    progress_format_line(&pt, pt.start_time + 2.0, line, sizeof(line));
    assert(strstr(line, "50.0%") != NULL);
    assert(strstr(line, "Transferred: 1/4 Files") != NULL);
    assert(strstr(line, "4 workers") != NULL);
    assert(strstr(line, "ETA 00:02") != NULL);
    progress_destroy(&pt);

    ScanProgress sp;
    scan_progress_init(&sp, 4, true);
    scan_progress_update(&sp, 12, 3, 2048);
    scan_progress_format_line(&sp, now_monotonic(), line, sizeof(line));
    assert(strstr(line, "Scanning [") != NULL);
    assert(strstr(line, "12 files | 3 dirs | 2.0 KB | 4 workers") != NULL);
    scan_progress_finish(&sp);
    scan_progress_format_line(&sp, now_monotonic(), line, sizeof(line));
    assert(strstr(line, "12 files | 3 dirs | 2.0 KB in ") != NULL);
    scan_progress_destroy(&sp);
}

static void test_end_to_end_local_copy(void) {
    char src_tmpl[] = "/tmp/scpc_e2e_src_XXXXXX";
    char dst_tmpl[] = "/tmp/scpc_e2e_dst_XXXXXX";
    char *src = mkdtemp(src_tmpl);
    char *dst = mkdtemp(dst_tmpl);
    assert(src != NULL && dst != NULL);

    char f1[512], f2[512], sub[512], f3[512];
    snprintf(sub, sizeof(sub), "%s/nested", src);
    assert(mkdir(sub, 0755) == 0);
    snprintf(f1, sizeof(f1), "%s/one.txt", src);
    snprintf(f2, sizeof(f2), "%s/two.txt", src);
    snprintf(f3, sizeof(f3), "%s/three.txt", sub);
    write_dummy_file(f1, 1024);
    write_dummy_file(f2, 2048);
    write_dummy_file(f3, 4096);

    char *argv[] = {"scpc", "-rq", "-w", "2", src, dst};
    ParsedArgs args;
    char err[256] = {0};
    assert(parse_args(6, argv, &args, err, sizeof(err)) == 0);
    int rc = run_scpc(&args);
    parsed_args_free(&args);
    assert(rc == 0);

    /* Verify copied files exist with exact sizes */
    const char *src_base = strrchr(src, '/') + 1;
    char c1[512], c2[512], c3[512], csub[512], croot[512];
    snprintf(croot, sizeof(croot), "%s/%s", dst, src_base);
    snprintf(csub, sizeof(csub), "%s/nested", croot);
    snprintf(c1, sizeof(c1), "%s/one.txt", croot);
    snprintf(c2, sizeof(c2), "%s/two.txt", croot);
    snprintf(c3, sizeof(c3), "%s/three.txt", csub);

    struct stat st;
    assert(stat(c1, &st) == 0 && st.st_size == 1024);
    assert(stat(c2, &st) == 0 && st.st_size == 2048);
    assert(stat(c3, &st) == 0 && st.st_size == 4096);

    /* Cleanup */
    unlink(c3);
    unlink(c2);
    unlink(c1);
    rmdir(csub);
    rmdir(croot);
    rmdir(dst);

    unlink(f3);
    unlink(f2);
    unlink(f1);
    rmdir(sub);
    rmdir(src);
}

int main(void) {
    test_cli_parsing();
    test_remote_path_detection();
    test_local_recursive_scan();
    test_determine_workers();
    test_progress_and_pty_parser();
    test_end_to_end_local_copy();
    printf("All scpc C tests passed!\n");
    return 0;
}

