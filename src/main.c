/**
 * HTTP echo / static files server
 * - non-blocking I/O (poll-based event loop)
 * - per-client state machine (read -> handle -> write)
 * - HTTP/1.1 keep-alive support
 * - basic request parsing (request line + headers)
 * - static file serving (GET + HEAD)
 * - simple routing (/, .html fallback, index.html)
 * - connection timeouts
 * - basic pipelining support
 *
  * Limitations:
 * - only HTTP/1.1 (no HTTP/2, no TLS)
 * - only GET+HEAD supported in fs mode (no POST, etc.)
 * - no "Transfer-Encoding: chunked"
 * - no request streaming (entire request buffered)
 * - no response streaming (entire file buffered in memory)
 * - no persistent caching (files read on each request)
 * - limited MIME type detection
 * - no range requests (partial content)
 * - no directory listing
 * - no URL decoding (%20 etc.), no query strings (?a=1)
 * - strict path validation (may reject some valid URLs)
 * - no concurrency beyond poll() (single-threaded)
 * - fixed limits:
 *     - max clients
 *     - max request size
 *     - max file size
 *
 * Security notes:
 * - prevents directory traversal ("..")
 * - restricts allowed path characters
 * - does not follow symlinks explicitly (depends on stat)
 * - not hardened for production use
 *
 * TODO:
 * - sendfile() / streaming responses for large files
 * - LRU file cache
 * - more complete MIME type handling
 * - configuration (port, limits, timeouts)
 * - `writev() + `struct iovec` (`<sys/uio.h>`) for concurrency-safe logs
 */

#include <stdio.h>      // printf(), perror()
#include <stdlib.h>     // exit(), EXIT_FAILURE
#include <string.h>     // memset()
#include <strings.h>    // strcasecmp()
#include <unistd.h>     // close()
#include <arpa/inet.h>  // htons(), htonl(), sockaddr_in, INADDR_ANY, inet_pton
#include <sys/socket.h> // socket(), bind(), listen(), setsockopt()
#include <signal.h>     // signal(), sigaction()
#include <errno.h>      // EINTR
#include <poll.h>       // poll()
#include <limits.h>     // INT_MAX, PATH_MAX
#include <fcntl.h>      // fcntl(), O_NONBLOCK
#include <time.h>       // timespec, clock_gettime
#include <sys/stat.h>   // stat()
#include <stdarg.h>     // va_list, va_start, va_end

#define PORT 8080       // Port the server will listen on
#define BACKLOG 10      // Max number of pending connections
#define MAX_CLIENTS 10  // Max number of active connections
#define MAX_REQUEST_SIZE (1024 * 1024) // 1 MB
#define MAX_HEADERS_SIZE (16 * 1024) // 16 KB
#define MAX_HEADERS_COUNT 100
#define MAX_HEADER_LINE 2048
#define MAX_FILE_SIZE (4 * 1024 * 1024) // 4 MB

typedef enum LogLevel {
    LOG_L_DUMP = 0,
    LOG_L_DEBUG,
    LOG_L_INFO,
    LOG_L_WARN,
    LOG_L_ERROR
} log_level_t;

typedef enum ServerMode {
    MODE_ECHO,
    MODE_FS
} server_mode_t;

typedef struct ServerConfig {
    server_mode_t mode;
    char* fs_root;   // NULL for echo
    int keep_alive_timeout_ms;
} server_config_t;

typedef enum ClientState {
    STATE_READING,
    STATE_HANDLING,
    STATE_WRITING,
    STATE_DONE,
    STATE_ERROR
} client_state_t;

typedef enum HttpStatus {
    HTTP_200_OK = 200,
    HTTP_400_BAD_REQUEST = 400,
    HTTP_403_FORBIDDEN = 403,
    HTTP_404_NOT_FOUND = 404,
    HTTP_405_NOT_ALLOWED = 405,
    HTTP_500_INTERNAL_ERROR = 500,
    HTTP_501_NOT_IMPLEMENTED = 501
} http_status_t;

typedef enum ParseResult {
    PARSE_INCOMPLETE,
    PARSE_OK,
    PARSE_ERROR,
    PARSE_NOT_SUPPORTED
} parse_result_t;

typedef enum Connection {
    CONN_CLOSE,
    CONN_KEEP_ALIVE
} conn_t;

typedef struct Buffer {
    char* data;
    int len;
    int cap;
} buffer_t;

typedef struct Request {
    int id;
    buffer_t buffer;

    int headers_len;
    int body_len;

    char method[8];
    char path[1024];
    char version[16];
    conn_t connection;
} request_t;

typedef struct Response {
    buffer_t buffer;
    int sent;

    int status_code;
    conn_t connection;
} response_t;

typedef struct Client {
    int id;
    client_state_t state;
    int fd;
    struct sockaddr_in addr;
    long last_activity_ms;
    int peer_closed;
    http_status_t error_code;

    request_t request;
    response_t response;
} client_t;

typedef enum ClientStepResult {
    CLIENT_RES_CLOSE,
    CLIENT_RES_KEEP
} client_res_t;

typedef struct pollfd pollfd_t;

static int next_client_id = 1;
static int next_request_id = 1;

long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000L) + (ts.tv_nsec / 1000000L);
}

const char* state_str(client_state_t s) {
    switch (s) {
    case STATE_READING: return "READ";
    case STATE_HANDLING: return "HANDL";
    case STATE_WRITING: return "WRITE";
    case STATE_DONE: return "DONE";
    case STATE_ERROR: return "ERROR";
    default: return "?";
    }
}

const char* connection_str(conn_t conn) {
    return conn == CONN_CLOSE ? "close" : "keep-alive";
}

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

// ----------------------------------------------
// Logs
// ----------------------------------------------

static log_level_t LOG_LEVEL = LOG_L_INFO;

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

#define LOG_CLIENT(level, client, fmt, ...) \
    log_msg(level, "[c=%d fd=%d req=%d state=%s]\t" fmt, \
        (client)->id, (client)->fd, (client)->request.id, \
        state_str((client)->state), ##__VA_ARGS__)

#define LOG_CLIENT_DUMP(client, fmt, ...) LOG_CLIENT(LOG_L_DUMP, client, fmt, ##__VA_ARGS__)
#define LOG_CLIENT_DEBUG(client, fmt, ...) LOG_CLIENT(LOG_L_DEBUG, client, fmt, ##__VA_ARGS__)
#define LOG_CLIENT_INFO(client, fmt, ...) LOG_CLIENT(LOG_L_INFO, client, fmt, ##__VA_ARGS__)
#define LOG_CLIENT_WARN(client, fmt, ...) LOG_CLIENT(LOG_L_WARN, client, fmt, ##__VA_ARGS__)
#define LOG_CLIENT_ERROR(client, fmt, ...) LOG_CLIENT(LOG_L_ERROR, client, fmt, ##__VA_ARGS__)

#define LOG_CLIENT_PERROR_LEVEL(level, client, fmt, ...) \
    LOG_PERROR_LEVEL(level, "[c=%d fd=%d req=%d state=%s]\t" fmt, \
        (client)->id, (client)->fd, (client)->request.id, \
        state_str((client)->state), ##__VA_ARGS__)

#define LOG_CLIENT_PERROR(client, fmt, ...) LOG_CLIENT_PERROR_LEVEL(LOG_L_ERROR, client, fmt, ##__VA_ARGS__)
#define LOG_CLIENT_PERROR_DUMP(client, fmt, ...) LOG_CLIENT_PERROR_LEVEL(LOG_L_DUMP, client, fmt, ##__VA_ARGS__)

// ----------------------------------------------
// Buffer helpers
// ----------------------------------------------

int buffer_init(buffer_t* buf, int size) {
    memset(buf, 0, sizeof(buffer_t));
    buf->data = malloc(size);
    if (!buf->data) return -1;
    buf->len = 0;
    buf->cap = size;
    return 1;
}

void buffer_free(buffer_t* buf) {
    if (buf->data) free(buf->data);
    memset(buf, 0, sizeof(buffer_t));
}

int buffer_reserve(buffer_t* buf, int needed) {
    int new_cap = buf->cap;

    while (new_cap < needed) {
        if (new_cap >= INT_MAX / 2) return -1;
        new_cap *= 2;
    }

    if (new_cap <= buf->cap) return 1;

    char* tmp = realloc(buf->data, new_cap);

    if (tmp == NULL) return -1;

    buf->data = tmp;
    buf->cap = new_cap;
    return 1;
}

// ----------------------------------------------
// Signal handlers
// ----------------------------------------------

/**
 * Global flag for graceful shutdown.
 *
 * volatile: prevents compiler from caching the value in a register,
 *           ensuring the signal handler's write is visible.
 * sig_atomic_t: guarantees atomic read/write operations,
 *                making it safe to use in signal handlers.
 */
volatile sig_atomic_t keep_running = 1;

/**
 * Signal handler for SIGINT and SIGTERM.
 *
 * When triggered, sets keep_running to 0 to signal the main
 * loop to exit. The (void) cast on signum avoids unused
 * parameter warning since we don't need the signal number.
 */
void signal_handler(int signum) {
    (void)signum;     // explicitly mark parameter as unused
    keep_running = 0; // stop the main loop
}

void setup_signal_handlers() {
    /**
     * Set up signal handlers for graceful shutdown
     *
     * SIGINT:  Sent when user presses Ctrl+C
     * SIGTERM: Sent by kill command (default for docker stop, etc.)
     *
     * When either signal is received, signal_handler sets
     * keep_running to 0, causing the main loop to exit.
     *
     *
     * Why not just signal()?
     * signal(SIGINT, signal_handler);
     * signal(SIGTERM, signal_handler);
     *  - signal() may restart syscalls like poll()/accept()
     *  - sigaction() gives precise control
     *
     * We intentionally DO NOT use SA_RESTART
     * so that poll() returns when interrupted by SIGINT.
     */
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);  // no additional signals blocked
    sa.sa_flags = 0;           // DO NOT use SA_RESTART
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        LOG_PERROR("sigaction failed: SIGINT | SIGTERM");
        exit(EXIT_FAILURE);
    }

    /**
     * If client closes connection / crashed / reset the socket
     * during sending response with `write()` -> kernel may send SIGPIPE
     * by default it terminates the process immediately
     * ignore it instead
     */
    struct sigaction sa_pipe;
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa_pipe, NULL) < 0) {
        LOG_PERROR("sigaction failed: SIGPIPE");
        exit(EXIT_FAILURE);
    }
}

// ----------------------------------------------
// Server setup
// ----------------------------------------------

/**
 * Set non-blocking mode for socket
 */
int set_nonblocking(int fd) {
    /**
     * Set non-blocking mode for socket
     * fcntl = "file control", get/modify properties of file descriptor
     * nonblock -> on read() and write()
     */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int setup_server() {
    /**
     * STEP 1: Create a socket
     *
     * socket(domain, type, protocol)
     *
     * AF_INET     -> IPv4
     * SOCK_STREAM -> TCP (reliable, connection-based)
     * 0           -> automatically select protocol (TCP)
     */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_PERROR("socket failed");
        exit(EXIT_FAILURE);
    }

    if (set_nonblocking(server_fd) < 0) {
        LOG_PERROR("fcntl server_fd");
        exit(EXIT_FAILURE);
    }

    /**
     * STEP 2: Set socket options
     *
     * setsockopt() allows configuration of socket behavior.
     *
     * SO_REUSEADDR:
     *   Allows reuse of local addresses (avoids "Address already in use")
     *   when restarting the server quickly.
     */
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_PERROR("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    /**
     * htons() converts 16-bit values to network byte order.
     * htonl() converts 32-bit values to network byte order.
     */
    address.sin_family = AF_INET; // IPv4
    address.sin_port = htons(PORT);

    // all available network interfaces => 0.0.0.0
    // address.sin_addr.s_addr = htonl(INADDR_ANY);

    // INADDR_LOOPBACK (localhost, 127.0.0.1) can cause portability issues
    // address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        LOG_PERROR("inet_pton failed");
        exit(EXIT_FAILURE);
    }

    /**
     * bind(socket, address, address_length)
     *
     * We cast sockaddr_in* to sockaddr* because the API is generic.
     */
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        LOG_PERROR("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    /**
     * STEP 4: Start listening for incoming connections
     *
     * listen(socket, backlog)
     *
     * backlog:
     *   Maximum number of pending connections in the queue
     */
    if (listen(server_fd, BACKLOG) < 0) {
        LOG_PERROR("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    return server_fd;
}

// ----------------------------------------------
// Main loop helpers
// ----------------------------------------------

/**
 * Free allocated memory for client
 */
void free_client(client_t* client) {
    if (client->request.buffer.data != NULL) {
        buffer_free(&client->request.buffer);
        memset(&client->request, 0, sizeof(request_t));
    }
    if (client->response.buffer.data != NULL) {
        buffer_free(&client->response.buffer);
        memset(&client->response, 0, sizeof(response_t));
    }
}

/**
 * Close client helper: close client socket, update sockets and clients arrays
 */
void close_client(pollfd_t* pfds, client_t* clients, int* nfds, int i) {
    LOG_CLIENT_INFO(&clients[i], "closing connection");

    close(pfds[i].fd);
    free_client(clients + i);

    pfds[i] = pfds[(*nfds) - 1];
    clients[i] = clients[(*nfds) - 1];

    (*nfds)--;
}

void set_client_state(client_t* client, client_state_t state) {
    LOG_CLIENT_DEBUG(client, "state -> %s", state_str(state));
    client->state = state;
}

void set_client_error(client_t* client, http_status_t status_code) {
    if (status_code < 100 || status_code > 599) {
        LOG_CLIENT_ERROR(client,
            "invalid status code %d -> forcing 500",
            status_code);
        status_code = HTTP_500_INTERNAL_ERROR;
    }

    client->error_code = status_code;
    LOG_CLIENT_ERROR(client, "set error: %d", status_code);

    set_client_state(client, STATE_HANDLING);
}

// ----------------------------------------------
// Read request
// ----------------------------------------------

/**
 * Read request from client socket
 * handles partial reads and interrupts
 */
void client_read_request(client_t* client) {
    request_t* req = &client->request;
    buffer_t* buf = &req->buffer;

    while (1) {
        // ensure buffer has space for at least 1 more byte
        if (buf->len + 1 >= buf->cap) {
            if (buf->cap > MAX_REQUEST_SIZE / 2) return set_client_error(client, HTTP_400_BAD_REQUEST);
            if (buffer_reserve(buf, buf->len + 1) < 0) return set_client_error(client, HTTP_500_INTERNAL_ERROR);
        }

        // read remaining capacity - 1 (reserve for '\0')
        ssize_t n = read(client->fd, buf->data + buf->len, buf->cap - buf->len - 1);

        if (n < 0) {
            LOG_CLIENT_PERROR_DUMP(client, "read");
            if (errno == EINTR) continue; // interrupted, can try read again immediately
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // read blocked, try in next poll iteration

            LOG_CLIENT_PERROR(client, "read");
            return set_client_error(client, HTTP_400_BAD_REQUEST);
        }

        if (n == 0) {
            client->peer_closed = 1;
            LOG_CLIENT_DUMP(client, "read EOF => peer_closed=1");
            break;
        }

        LOG_CLIENT_DUMP(client, "read %d bytes", n);
        buf->len += n;
        buf->data[buf->len] = '\0';
        client->last_activity_ms = now_ms();
    }
}

// ----------------------------------------------
// Parse request
// ----------------------------------------------

/**
 * Parse request line, headers, populate `client->request`
 */
parse_result_t parse_request_headers(client_t* client) {
    char* buf = client->request.buffer.data;

    /**
     * Parse request line
     */
    char* line_end = strstr(buf, "\r\n");
    if (!line_end) return PARSE_ERROR;

    *line_end = '\0';

    if (sscanf(buf,
        "%7s %1023s %15s",
        client->request.method,
        client->request.path,
        client->request.version
    ) != 3) return PARSE_ERROR;

    *line_end = '\r';

    client->request.connection = CONN_CLOSE;
    if (strcmp(client->request.version, "HTTP/1.1") == 0) {
        client->request.connection = CONN_KEEP_ALIVE;
    }

    /**
     * Parse headers
     */
    char* p = line_end + 2;
    char* headers_end = client->request.buffer.data + client->request.headers_len;

    client->request.body_len = 0;

    int headers_count = 0;

    while (p < headers_end - 2) {
        char* next = strstr(p, "\r\n");
        if (!next) return PARSE_ERROR;

        if (next == p) break; // empty line

        headers_count++;
        if (headers_count > MAX_HEADERS_COUNT) return PARSE_ERROR;

        *next = '\0';

        char key[MAX_HEADER_LINE];
        char value[MAX_HEADER_LINE];

        if (sscanf(p, "%[^:]: %2047[^\r\n]", key, value) == 2) {

            if (strcasecmp(key, "content-length") == 0) {
                char* end;
                long len = strtol(value, &end, 10);
                if (*end != '\0' || len < 0 || len > MAX_REQUEST_SIZE) return PARSE_ERROR;
                client->request.body_len = (int)len;
            }
            else if (strcasecmp(key, "connection") == 0) {
                if (strcasecmp(value, "keep-alive") == 0) {
                    client->request.connection = CONN_KEEP_ALIVE;
                }
                else if (strcasecmp(value, "close") == 0) {
                    client->request.connection = CONN_CLOSE;
                }
            }
            else if (strcasecmp(key, "transfer-encoding") == 0) {
                return PARSE_NOT_SUPPORTED; // not supported
            }
        }

        *next = '\r';
        p = next + 2;
    }

    return PARSE_OK;
}

parse_result_t try_parse_request(client_t* client) {
    request_t* req = &client->request;
    buffer_t* buf = &req->buffer;

    if (req->headers_len > 0) {
        int body_received = buf->len - req->headers_len;
        if (body_received < req->body_len) return PARSE_INCOMPLETE;

        return PARSE_OK;
    }

    char* headers_end = strstr(buf->data, "\r\n\r\n");

    if (!headers_end) {
        if (buf->len > MAX_HEADERS_SIZE) return PARSE_ERROR;
        return PARSE_INCOMPLETE;
    }

    req->headers_len = headers_end + 4 - buf->data;
    req->id = next_request_id++;

    return parse_request_headers(client);
}

void client_parse_request(client_t* client) {
    if (client->state != STATE_READING) return;

    parse_result_t r = try_parse_request(client);
    LOG_CLIENT_DUMP(client, "parse result = %d", r);

    if (r == PARSE_INCOMPLETE) {
        if (client->peer_closed) {
            if (client->request.buffer.len < 1) return set_client_state(client, STATE_DONE);
            return set_client_error(client, HTTP_400_BAD_REQUEST);
        }
        return;
    };

    if (r == PARSE_ERROR) return set_client_error(client, HTTP_400_BAD_REQUEST);

    if (r == PARSE_NOT_SUPPORTED) return set_client_error(client, HTTP_405_NOT_ALLOWED);

    LOG_CLIENT_INFO(client,
        "request: %s %s (headers=%d body=%d conn=%s)",
        client->request.method, client->request.path,
        client->request.headers_len, client->request.body_len, connection_str(client->request.connection));

    LOG_CLIENT_DUMP(client,
        "request:\n---\n"
        "%.*s"
        "\n---",
        client->request.headers_len + client->request.body_len, client->request.buffer.data
    );

    set_client_state(client, STATE_HANDLING);
}

// ----------------------------------------------
// Build response
// ----------------------------------------------

int response_build(
    response_t* res,
    http_status_t status_code,
    const char* status_text,
    const char* content_type,
    const char* body,
    int body_len,
    conn_t connection
) {
    int header_cap = 1024;
    int total_cap = header_cap + body_len;
    if (!buffer_init(&res->buffer, total_cap)) return 0;

    int header_len = snprintf(
        res->buffer.data,
        header_cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status_code,
        status_text,
        content_type ? content_type : "application/octet-stream",
        body_len,
        connection_str(connection)
    );

    if (header_len < 0 || header_len >= header_cap) {
        buffer_free(&res->buffer);
        return 0;
    }

    if (body_len > 0 && body != NULL) {
        memcpy(res->buffer.data + header_len, body, body_len);
    }

    res->buffer.len = header_len + body_len;
    res->sent = 0;
    res->status_code = status_code;
    res->connection = connection;

    return 1;
}

int create_error_response(client_t* client, http_status_t code) {
    const char* text = "Internal Server Error";

    switch (code) {
    case HTTP_400_BAD_REQUEST: text = "Bad Request"; break;
    case HTTP_403_FORBIDDEN: text = "Forbidden"; break;
    case HTTP_404_NOT_FOUND: text = "Not Found"; break;
    case HTTP_405_NOT_ALLOWED: text = "Method Not Allowed"; break;
    case HTTP_501_NOT_IMPLEMENTED: text = "Not Implemented"; break;
    default: break;
    }

    return response_build(
        &client->response,
        code,
        text,
        "text/plain",
        NULL,
        0,
        1 // always close on error
    );
}

// ----------------------------------------------
// Echo response
// ----------------------------------------------

void create_echo_response(client_t* client) {
    int body_len = client->request.body_len;
    const char* body = client->request.buffer.data + client->request.headers_len;

    if (!body) body_len = 0;

    if (!response_build(
        &client->response,
        HTTP_200_OK,
        "OK",
        "text/plain",
        body,
        body_len,
        client->request.connection
    )) set_client_error(client, HTTP_500_INTERNAL_ERROR);
}

// ----------------------------------------------
// FS response
// ----------------------------------------------

int is_valid_path(const char* path) {
    // not empty and starts with "/"
    if (!path || strlen(path) < 1 || path[0] != '/') return 0;

    // allow only safe characters
    for (const char* p = path; *p; p++) {
        char c = *p;
        if (
            !(c >= 'a' && c <= 'z')
            && !(c >= 'A' && c <= 'Z')
            && !(c >= '0' && c <= '9')
            && c != '/'
            && c != '.'
            && c != '-'
            && c != '_'
            ) return 0;
    }

    // reject ".." and double slash
    if (strstr(path, "..") || strstr(path, "//")) return 0;

    return 1;
}

int file_exists(const char* path, int* is_dir) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) return 1;
    return 0;
}

int resolve_path(char* out, size_t cap, server_config_t* config, const char* url_path) {
    char path[PATH_MAX];
    int is_dir = 0;
    int n = 0;

    // Case 1: "/"
    if (url_path[0] == '/' && url_path[1] == '\0') {
        n = snprintf(path, sizeof(path), "%s/index.html", config->fs_root);
        if (n < 0 || (size_t)n >= sizeof(path)) return 0;

        if (file_exists(path, &is_dir) && !is_dir) {
            n = snprintf(out, cap, "%s", path);
            if (n < 0 || (size_t)n >= cap) return 0;
            return 1;
        }

        return 0;
    }

    // build base path, strip leading '/'
    const char* rel = url_path + 1;
    n = snprintf(path, sizeof(path), "%s/%s", config->fs_root, rel);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;

    // Case 2: ends with "/"
    size_t len = strlen(url_path);
    if (url_path[len - 1] == '/') {
        char with_index[PATH_MAX];
        n = snprintf(with_index, sizeof(with_index), "%s/index.html", path);
        if (n < 0 || (size_t)n >= sizeof(with_index)) return 0;

        if (file_exists(with_index, &is_dir) && !is_dir) {
            n = snprintf(out, cap, "%s", with_index);
            if (n < 0 || (size_t)n >= cap) return 0;
            return 1;
        }

        return 0;
    }

    // Case 3: try exact file
    if (file_exists(path, &is_dir) && !is_dir) {
        n = snprintf(out, cap, "%s", path);
        if (n < 0 || (size_t)n >= cap) return 0;
        return 1;
    }

    // Case 4: try ".html"
    char with_html[PATH_MAX];
    n = snprintf(with_html, sizeof(with_html), "%s.html", path);
    if (n < 0 || (size_t)n >= sizeof(with_html)) return 0;
    if (file_exists(with_html, &is_dir) && !is_dir) {
        n = snprintf(out, cap, "%s", with_html);
        if (n < 0 || (size_t)n >= cap) return 0;
        return 1;
    }

    // Case 5: try directory index
    char with_index[PATH_MAX];
    n = snprintf(with_index, sizeof(with_index), "%s/index.html", path);
    if (n < 0 || (size_t)n >= sizeof(with_index)) return 0;
    if (file_exists(with_index, &is_dir) && !is_dir) {
        n = snprintf(out, cap, "%s", with_index);
        if (n < 0 || (size_t)n >= cap) return 0;
        return 1;
    }

    return 0;
}

const char* get_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    ext++; // skip '.'

    if (strcasecmp(ext, "html") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, "css") == 0) return "text/css";
    if (strcasecmp(ext, "js") == 0) return "application/javascript";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "txt") == 0) return "text/plain; charset=utf-8";

    return "application/octet-stream";
};

int read_file(const char* path, char** out_buf, int* out_len) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;

    if (!S_ISREG(st.st_mode)) return 0;

    if (st.st_size < 0 || st.st_size > MAX_FILE_SIZE) return 0;

    size_t size = st.st_size;

    FILE* f = fopen(path, "rb");
    if (!f) return 0;

    char* buf = malloc(size);
    if (!buf) { fclose(f); return 0; }

    size_t read_bytes = fread(buf, 1, size, f);
    fclose(f);

    if (read_bytes != size) { free(buf); return 0; }

    *out_buf = buf;
    *out_len = size;
    return 1;
}

void create_fs_response(client_t* client, server_config_t* config) {
    if (
        strcmp(client->request.method, "GET") != 0
        && strcmp(client->request.method, "HEAD") != 0
        ) {
        return set_client_error(client, HTTP_405_NOT_ALLOWED);
    }
    if (!is_valid_path(client->request.path)) {
        return set_client_error(client, HTTP_400_BAD_REQUEST);
    }

    char resolved[PATH_MAX];

    if (!resolve_path(resolved, sizeof(resolved), config, client->request.path)) {
        return set_client_error(client, HTTP_404_NOT_FOUND);
    }

    char* file_data = NULL;
    int file_len = 0;

    if (!read_file(resolved, &file_data, &file_len)) {
        return set_client_error(client, HTTP_403_FORBIDDEN);
    }

    const char* mime = get_mime_type(resolved);
    int is_head_req = strcmp(client->request.method, "HEAD") == 0;

    if (!response_build(
        &client->response,
        HTTP_200_OK,
        "OK",
        mime,
        is_head_req ? NULL : file_data,
        file_len,
        client->request.connection
    )) set_client_error(client, HTTP_500_INTERNAL_ERROR);

    free(file_data);
    return;
}

// ----------------------------------------------
// Handle request
// ----------------------------------------------

void client_handle_request(client_t* client, server_config_t* config) {
    if (client->error_code == 0) {
        if (config->mode == MODE_ECHO) {
            create_echo_response(client);
        }
        else {
            create_fs_response(client, config);
        }
    }

    if (client->error_code != 0) {
        if (!create_error_response(client, client->error_code)) {
            return set_client_state(client, STATE_ERROR);
        }
    }

    LOG_CLIENT_INFO(client,
        "response: %d (%d bytes, conn=%s)",
        client->response.status_code, client->response.buffer.len, connection_str(client->response.connection));

    LOG_CLIENT_DUMP(client,
        "response:\n---\n"
        "%.*s"
        "\n---",
        client->response.buffer.len, client->response.buffer.data
    );

    set_client_state(client, STATE_WRITING);
}

// ----------------------------------------------
// Send response
// ----------------------------------------------

/**
 * Write response to client socket
 * handles partial writes and interrupts
 */
void client_write_response(client_t* client) {
    response_t* res = &client->response;
    buffer_t* buf = &res->buffer;

    while (res->sent < buf->len) {
        LOG_CLIENT_DUMP(client, "write progress: %d/%d bytes", res->sent, buf->len);

        // write next bytes
        ssize_t n = write(
            client->fd,
            buf->data + res->sent,
            buf->len - res->sent
        );

        LOG_CLIENT_DUMP(client, "wrote %zd bytes", n);

        if (n <= 0) {
            LOG_CLIENT_PERROR_DUMP(client, "write");
            if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            else if (errno == EPIPE) {
                LOG_CLIENT_DEBUG(client, "write errno EPIPE");
                return set_client_state(client, STATE_DONE);
            }
            else {
                LOG_CLIENT_PERROR(client, "write");
                return set_client_state(client, STATE_ERROR);
            }
        }

        res->sent += n;
        client->last_activity_ms = now_ms();
    }

    set_client_state(client, STATE_DONE);
    buffer_free(buf);
}

// ----------------------------------------------
// Post-write
// ----------------------------------------------

void reset_client_for_next_request(client_t* client) {
    request_t* req = &client->request;

    int remaining = req->buffer.len - req->headers_len - req->body_len;
    if (remaining < 0) remaining = 0;

    // Shift request buffer
    if (remaining > 0) {
        memmove(
            req->buffer.data,
            req->buffer.data + req->headers_len + req->body_len,
            remaining
        );
    }
    req->buffer.len = remaining;
    req->buffer.data[req->buffer.len] = '\0';

    // Reset request
    req->headers_len = 0;
    req->body_len = 0;
    req->id = 0;
    req->method[0] = '\0';
    req->path[0] = '\0';
    req->version[0] = '\0';

    // Reset response
    memset(&client->response, 0, sizeof(response_t));

    // Reset client
    client->error_code = 0;
    client->last_activity_ms = now_ms();
}

/**
 * if finished writing response and connection is still active,
 * reset client to prepare for the next request and try to
 * parse request already in buffer (request pipelining)
 */
void client_post_write(client_t* client) {
    if (client->state != STATE_DONE || client->request.connection == CONN_CLOSE) return;

    LOG_CLIENT_DEBUG(client, "post-write reset for new request");
    reset_client_for_next_request(client);

    if (client->peer_closed && client->request.buffer.len == 0) {
        LOG_CLIENT_DEBUG(client, "peer closed, no more data");
    }
    else {
        LOG_CLIENT_DEBUG(client, "moving to next request");
        set_client_state(client, STATE_READING);
        client_parse_request(client);
    }
}

// ----------------------------------------------
// Main loop
// ----------------------------------------------

void accept_client_connection(int server_fd, pollfd_t* pfds, client_t* clients, int* nfds_ptr) {
    LOG_DEBUG("new connection c=>%d", next_client_id);

    /**
     * What accept() does
     *  1. Takes one connection from the queue
     *  2. Creates a new socket (client_fd)
     *  3. Returns client address info (optional)
     *
     * Important behavior: Blocking call by default → waits until a client connects
     */

    struct sockaddr_in client_addr = { 0 };
    socklen_t client_addr_len = sizeof(client_addr);
    int client_fd = accept(
        server_fd,
        (struct sockaddr*)&client_addr,
        &client_addr_len
    );
    if (client_fd < 0) {
        if (errno != EINTR) LOG_PERROR("[c=%d] accept", next_client_id);
        return; // don't kill server, go to next connection
    }

    if (set_nonblocking(client_fd) < 0) {
        LOG_PERROR("[c=%d] fcntl failed", next_client_id);
        close(client_fd);
        return;
    }

    int nfds = *nfds_ptr;

    if (nfds >= MAX_CLIENTS + 1) {
        LOG_WARN("[c=%d] too many clients, dropping connection", next_client_id);
        close(client_fd);
        return;
    }

    pfds[nfds].fd = client_fd;

    memset(&clients[nfds], 0, sizeof(client_t));
    clients[nfds].id = next_client_id++;
    clients[nfds].state = STATE_READING;
    clients[nfds].fd = client_fd;
    clients[nfds].addr = client_addr;
    clients[nfds].last_activity_ms = now_ms();
    clients[nfds].peer_closed = 0;
    clients[nfds].error_code = 0;

    memset(&clients[nfds].request, 0, sizeof(request_t));
    if (buffer_init(&clients[nfds].request.buffer, 4096) < 0) {
        LOG_ERROR("[c=%d] failed to allocate request buffer", next_client_id);
        close(client_fd);
        return;
    }
    clients[nfds].request.connection = CONN_KEEP_ALIVE; // default HTTP/1.1 behavior
    clients[nfds].request.headers_len = 0;

    memset(&clients[nfds].response, 0, sizeof(response_t));

    LOG_CLIENT_DEBUG(&clients[nfds], "connection accepted");

    (*nfds_ptr)++;
}

client_res_t client_step(pollfd_t* pfds, client_t* clients, int i, server_config_t* config) {
    client_t* client = &clients[i];
    LOG_CLIENT_DEBUG(client, "client ready");

    /**
     * Client read/write ready
     *
     * After accept
     *  - read from client_fd
     *  - write to client_fd
     *  - close client_fd when done
     */

    while (1) {
        if (client->state == STATE_READING) {
            if (!(pfds[i].revents & POLLIN)) break;
            pfds[i].revents &= ~POLLIN; // consume event

            LOG_CLIENT_DEBUG(client, "read ready");
            int prev_buff_len = client->request.buffer.len;

            client_read_request(client);
            client_parse_request(client);

            // read is blocked
            if (client->state == STATE_READING && client->request.buffer.len == prev_buff_len) break;
        }

        else if (client->state == STATE_HANDLING) {
            LOG_CLIENT_DEBUG(client, "handling");
            client_handle_request(client, config);
        }

        else if (client->state == STATE_WRITING) {
            LOG_CLIENT_DEBUG(client, "write ready");
            int prev_sent = client->response.sent;

            client_write_response(client);
            client_post_write(client);

            // write is blocked
            if (client->state == STATE_WRITING && client->response.sent == prev_sent) break;
        }

        else break;
    }

    if (client->state == STATE_DONE || client->state == STATE_ERROR) {
        if (client->state == STATE_DONE) LOG_CLIENT_DEBUG(client, "done");
        else LOG_CLIENT_DEBUG(client, "error");
        return CLIENT_RES_CLOSE;
    }

    return CLIENT_RES_KEEP;
}

void start_server(server_config_t* config) {
    setup_signal_handlers();

    int server_fd = setup_server();

    LOG_INFO("mode=%s, log-level=%d, timeout=%dms",
        config->mode == MODE_ECHO ? "echo" : "fs", LOG_LEVEL, config->keep_alive_timeout_ms);
    LOG_INFO("Server is listening on port %d...", PORT);

    /**
     * At this point, the server is ready to accept connections.
     * Next step would be:
     *   accept()
     * to handle incoming clients.
     */

     /**
      * poll() setup
      *
      * pollfd structure:
      *   fd      -> file descriptor
      *   events  -> what we want to watch (e.g., POLLIN)
      *   revents -> what actually happened
      */
    pollfd_t pfds[MAX_CLIENTS + 1];
    client_t clients[MAX_CLIENTS + 1];

    // Index 0 is always the server socket
    pfds[0].fd = server_fd;
    clients[0] = (client_t){ 0 };

    int nfds = 1;  // number of active fds

    while (keep_running) {
        // server_fd → listening socket (stays open)
        // client_fd → new socket for each client

        /**
         * Set events to watch (poll) for
         */
        pfds[0].events = POLLIN;
        // Sync events with state
        for (int i = 1; i < nfds; i++) {
            client_t* client = &clients[i];
            if (client->state == STATE_READING) pfds[i].events = POLLIN;
            else if (client->state == STATE_WRITING) pfds[i].events = POLLOUT;
            else pfds[i].events = 0;
        }

        /**
         * get min_timeout for poll
         */
        long now = now_ms();
        long min_timeout = config->keep_alive_timeout_ms;

        for (int i = 1; i < nfds; i++) {
            long idle = now - clients[i].last_activity_ms;
            long remaining = config->keep_alive_timeout_ms - idle;
            if (remaining < min_timeout) {
                min_timeout = remaining;
            }
        }

        if (min_timeout < 0) min_timeout = 0;

        /**
         * Use poll() before accept() to allow signal interruption.
         * - waits until one of the fds is ready
         * - or interrupted by signal (EINTR)
         *
         * poll() monitors the server socket for incoming connections.
         * - pfd.fd: the file descriptor to monitor
         * - pfd.events: POLLIN means "ready for reading" (connection waiting)
         * - timeout -1: wait indefinitely until a connection arrives or a
         *               signal interrupts it
         *
         * Why poll() instead of just accept()?
         *   When a signal (SIGINT/SIGTERM) arrives during accept(), the
         *   behavior is system-dependent. On some systems, accept() may
         *   not return until a connection arrives. Using poll() first
         *   ensures we check the keep_running flag when the signal arrives.
         */
        if (poll(pfds, nfds, min_timeout) < 0) {
            /*
             * EINTR: A signal interrupted the poll() call.
             * This happens when SIGINT/SIGTERM is received.
             * Break out of the loop to allow graceful shutdown.
             */
            if (errno != EINTR) LOG_PERROR("poll");
            break;
        }

        /**
         * close timed-out connections
         */
        long now2 = now_ms();

        for (int i = 1; i < nfds; i++) {
            long idle = now2 - clients[i].last_activity_ms;
            if (idle >= config->keep_alive_timeout_ms) {
                LOG_CLIENT_WARN(&clients[i], "timeout");
                close_client(pfds, clients, &nfds, i);
                i--;
            }
        }

        // Check all file descriptors
        for (int i = 0; i < nfds; i++) {
            if (pfds[i].revents == 0) continue;

            /**
             * New incoming connection
             */
            if (pfds[i].fd == server_fd) {
                if (!(pfds[i].revents & POLLIN)) continue;

                accept_client_connection(server_fd, pfds, clients, &nfds);
                continue;
            }

            /**
             * Socket error
             */
            if (pfds[i].revents & (POLLERR | POLLNVAL)) {
                client_t* client = &clients[i];
                if (pfds[i].revents & POLLERR) LOG_CLIENT_ERROR(client, "socket error: POLLERR");
                else LOG_CLIENT_ERROR(client, "socket error: POLLNVAL");
                close_client(pfds, clients, &nfds, i);
                i--;
                continue;
            }

            client_res_t res = client_step(pfds, clients, i, config);

            if (res == CLIENT_RES_CLOSE) {
                close_client(pfds, clients, &nfds, i);
                i--;
            }
        }
    }

    /**
     * Cleanup
     *
     * Close all open sockets (server and client sockets) and print shutdown message.
     * This code is now reachable thanks to the graceful shutdown
     * mechanism (signal_handler + poll() + keep_running).
     */
    LOG_INFO("shutting down server...");
    for (int i = 0; i < nfds; i++) {
        close(pfds[i].fd);
        free_client(clients + i);
    }
    LOG_INFO("server shutdown.");
}

// ----------------------------------------------
// Entrypoint
// ----------------------------------------------

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

    config.keep_alive_timeout_ms = 5000;

    start_server(&config);

    if (config.fs_root) free(config.fs_root);

    return 0;
}
