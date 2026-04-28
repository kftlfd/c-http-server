#pragma once

#include <arpa/inet.h>  // sockaddr_in

#include "http.h"

typedef enum ServerMode {
    MODE_ECHO,
    MODE_FS
} server_mode_t;

typedef struct ServerConfig {
    int port;
    server_mode_t mode;
    char* fs_root;   // NULL for echo
    int keep_alive_timeout_ms;
} server_config_t;

void start_server(server_config_t* config);
