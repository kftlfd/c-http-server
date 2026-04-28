#include <stdio.h>      // stderr, va_list
#include <string.h>     // strcmp
#include <time.h>       // timespec, clock_gettime
#include <stdarg.h>     // va_list, va_start, va_end

#include "log.h"

log_level_t LOG_LEVEL = LOG_L_INFO;

const char* log_level_str(log_level_t level) {
    switch (level) {
    case LOG_L_DUMP: return "DUMP";
    case LOG_L_DEBUG: return "DEBUG";
    case LOG_L_INFO: return "INFO";
    case LOG_L_WARN: return "WARN";
    case LOG_L_ERROR: return "ERROR";
    default: return "?";
    }
}

int parse_log_level(const char* s, log_level_t* out) {
    if (strcmp(s, "dump") == 0) { *out = LOG_L_DUMP; return 1; }
    if (strcmp(s, "debug") == 0) { *out = LOG_L_DEBUG; return 1; }
    if (strcmp(s, "info") == 0) { *out = LOG_L_INFO; return 1; }
    if (strcmp(s, "warn") == 0) { *out = LOG_L_WARN; return 1; }
    if (strcmp(s, "error") == 0) { *out = LOG_L_ERROR; return 1; }
    return 0;
}

void get_ts_local(struct timespec* ts, char* out, int out_len) {
    struct tm tm;
    localtime_r(&ts->tv_sec, &tm);

    size_t n = strftime(out, out_len, "%Y-%m-%dT%H:%M:%S", &tm);
    n += snprintf(out + n, out_len - n, ".%03ld", ts->tv_nsec / 1000000);
    strftime(out + n, out_len - n, "%z", &tm);
}

void get_ts_utc(struct timespec* ts, char* out, int out_len) {
    struct tm tm;
    gmtime_r(&ts->tv_sec, &tm);

    size_t n = strftime(out, out_len, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(out + n, out_len - n, ".%03ldZ", ts->tv_nsec / 1000000);
}

void log_msg(log_level_t level, const char* fmt, ...) {
    if (level < LOG_LEVEL) return;

    const char* level_str = log_level_str(level);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    char time[64];
    get_ts_local(&ts, time, 64);

    fprintf(stderr, "[%s] [%s]\t", time, level_str);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
}
