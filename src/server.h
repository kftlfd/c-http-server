#pragma once

#include <arpa/inet.h>  // sockaddr_in

#include "http.h"
#include "log.h"

typedef enum ServerMode {
    MODE_ECHO,
    MODE_FS
} server_mode_t;

typedef struct ServerConfig {
    server_mode_t mode;
    char host[64];  // "127.0.0.1"
    int port;       // 8080
    char* fs_root;  // NULL for echo
    int keep_alive_timeout_ms;
    log_level_t log_level;
} server_config_t;

void start_server(server_config_t* config);
