#include <stdio.h>      // printf(), perror()
#include <stdlib.h>     // exit()
#include <string.h>     // strcmp()
#include <sys/stat.h>   // stat()
#include <getopt.h>

#include "log.h"
#include "server.h"

#define SERVER_VERSION "1.0.0"

typedef enum CliResult {
    CLI_OK,
    CLI_VERSION,
    CLI_HELP,
    CLI_ERROR
} cli_result_t;

cli_result_t parse_cli(int argc, char** argv, server_config_t* config) {
    // ---- defaults ----
    memset(config, 0, sizeof(*config));

    strcpy(config->host, "127.0.0.1");
    config->port = 8000;
    config->keep_alive_timeout_ms = 5000;
    config->log_level = LOG_L_INFO;

    config->mode = MODE_ECHO;
    config->fs_root = NULL;

    // ---- options ----
    static struct option long_opts[] = {
        {"version", no_argument,       0, 'v' },
        {"help",    no_argument,       0, 'h' },
        {"port",    required_argument, 0, 'p'},
        {"host",    required_argument, 0, 'H'},
        {"log",     required_argument, 0, 'l'},
        {0, 0, 0, 0}
    };

    int opt;

    while ((opt = getopt_long(argc, argv, "vhH:p:l:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': {
            char* end;
            long p = strtol(optarg, &end, 10);
            if (*end != '\0' || p <= 0 || p > 65535) {
                fprintf(stderr, "Invalid port: %s\n", optarg);
                return CLI_ERROR;
            }
            config->port = (int)p;
            break;
        }

        case 'H':
            if (strlen(optarg) >= sizeof(config->host)) {
                fprintf(stderr, "Host too long\n");
                return CLI_ERROR;
            }
            strcpy(config->host, optarg);
            break;

        case 'l':
            if (!parse_log_level(optarg, &config->log_level)) {
                fprintf(stderr, "Invalid log level: %s\n", optarg);
                return CLI_ERROR;
            }
            break;

        case 'h': // --help
            return CLI_HELP;

        case 'v': // --version
            return CLI_VERSION;

        default:
            fprintf(stderr, "Try --help\n");
            return CLI_ERROR;
        }
    }

    // ---- positional args ----
    if (optind < argc) {
        if (strcmp(argv[optind], "echo") == 0) {
            config->mode = MODE_ECHO;
        }
        else if (strcmp(argv[optind], "fs") == 0) {
            config->mode = MODE_FS;

            if (optind + 1 >= argc) {
                fprintf(stderr, "fs mode requires directory\n");
                return CLI_ERROR;
            }

            config->fs_root = strdup(argv[optind + 1]);
            if (!config->fs_root) {
                perror("strdup");
                return CLI_ERROR;
            }

            struct stat st;
            if (stat(config->fs_root, &st) < 0 || !S_ISDIR(st.st_mode)) {
                fprintf(stderr, "Invalid directory: %s\n", config->fs_root);
                return CLI_ERROR;
            }
        }
        else {
            fprintf(stderr, "Unknown mode: %s\n", argv[optind]);
            return CLI_ERROR;
        }
    }

    return CLI_OK;
}

void free_server_config(server_config_t* config) {
    if (config->fs_root) free(config->fs_root);
}

void print_version() {
    printf("server version %s\n", SERVER_VERSION);
}

void print_help(const char* prog) {
    printf(
        "Usage: %s [echo | fs <dir>] [options]\n"
        "\n"
        "Modes:\n"
        "  echo               Run echo server (default)\n"
        "  fs <dir>           Serve static files from directory\n"
        "\n"
        "Options:\n"
        "  -v  --version      Show version\n"
        "  -h  --help         Show this help message\n"
        "\n"
        "  -H, --host <addr>  Bind address (default: 127.0.0.1)\n"
        "  -p, --port <port>  Port number (default: 8000)\n"
        "  -l, --log <level>  Log level: dump, debug, info, warn, error\n"
        "\n",
        prog
    );
}

int main(int argc, char** argv) {
    int exit_code = 0;
    server_config_t config;

    cli_result_t res = parse_cli(argc, argv, &config);
    LOG_LEVEL = config.log_level;

    switch (res) {
    case CLI_VERSION:
        print_version();
        break;

    case CLI_HELP:
        print_help(argv[0]);
        break;

    case CLI_OK:
        start_server(&config);
        break;

    default:
        exit_code = 1;
    }

    free_server_config(&config);
    return exit_code;
}
