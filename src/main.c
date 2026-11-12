#include "scpc.h"

#include <stdio.h>

int main(int argc, char **argv) {
    ParsedArgs args;
    char errbuf[512] = {0};

    if (parse_args(argc, argv, &args, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "scpc: %s\n", errbuf);
        return 1;
    }

    if (args.show_help) {
        print_help();
        parsed_args_free(&args);
        return 0;
    }

    int rc = run_scpc(&args);
    parsed_args_free(&args);
    return rc;
}

