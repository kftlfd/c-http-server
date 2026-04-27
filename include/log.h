#pragma once

typedef enum LogLevel {
    LOG_L_DUMP = 0,
    LOG_L_DEBUG,
    LOG_L_INFO,
    LOG_L_WARN,
    LOG_L_ERROR
} log_level_t;

extern log_level_t LOG_LEVEL;

int parse_log_level(const char* s, log_level_t* out);

void log_msg(log_level_t level, const char* fmt, ...);

#define LOG(level, fmt, ...) \
    log_msg(level, fmt, ##__VA_ARGS__)

#define LOG_DUMP(fmt, ...) log_msg(LOG_L_DUMP, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) log_msg(LOG_L_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) log_msg(LOG_L_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) log_msg(LOG_L_WARN, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_msg(LOG_L_ERROR, fmt, ##__VA_ARGS__)

#define LOG_PERROR_LEVEL(level, fmt, ...) \
    do { \
        int err = errno; \
        log_msg(level, fmt ": %s", ##__VA_ARGS__, strerror(err)); \
    } while (0)

#define LOG_PERROR(fmt, ...) LOG_PERROR_LEVEL(LOG_L_ERROR, fmt, ##__VA_ARGS__)
