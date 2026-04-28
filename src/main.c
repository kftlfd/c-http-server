#include <stdio.h>      // printf(), perror()
#include <stdlib.h>     // exit()
#include <string.h>     // strcmp()
#include <sys/stat.h>   // stat()

#include "log.h"
#include "server.h"

int main(int argc, char** argv) {
    server_config_t config = { 0 };

    if (argc < 2) {
        fprintf(stderr, "Usage: %s echo | fs <dir> [--log=dump|debug|info|warn|error]\n", argv[0]);
        exit(1);
    }

    if (strcmp(argv[1], "echo") == 0) {
        config.mode = MODE_ECHO;
    }
    else if (strcmp(argv[1], "fs") == 0) {
        config.mode = MODE_FS;
        if (argc < 3) {
            printf("Usage: %s fs <dir>\n", argv[0]);
            exit(1);
        }
        config.fs_root = strdup(argv[2]);
        if (!config.fs_root) {
            perror("fs_root strdup");
            exit(1);
        }
        struct stat st;
        if (stat(config.fs_root, &st) < 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "Invalid dir: %s\n", config.fs_root);
            exit(1);
        }
    }
    else {
        fprintf(stderr, "Unknown command\n");
        exit(1);
    }

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--log=", 6) == 0) {
            if (!parse_log_level(argv[i] + 6, &LOG_LEVEL)) {
                fprintf(stderr, "Invalid log level\n");
                exit(1);
            }
        }
    }

    config.port = 8080;
    config.keep_alive_timeout_ms = 5000;

    start_server(&config);

    if (config.fs_root) free(config.fs_root);

    return 0;
}
