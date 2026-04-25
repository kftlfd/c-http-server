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
 * - no URL decoding (%20 etc.)
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
 */

#include <stdio.h>      // printf(), perror()
#include <stdlib.h>     // exit(), EXIT_FAILURE
#include <string.h>     // memset()
#include <unistd.h>     // close()
#include <arpa/inet.h>  // htons(), htonl(), sockaddr_in, INADDR_ANY
#include <sys/socket.h> // socket(), bind(), listen(), setsockopt()
#include <signal.h>     // signal(), sigaction()
#include <errno.h>      // EINTR
#include <poll.h>       // poll()
#include <limits.h>     // INT_MAX
#include <fcntl.h>      // fcntl(), O_NONBLOCK
#include <time.h>       // timespec, clock_gettime
#include <sys/stat.h>  // stat()

#define PORT 8080       // Port the server will listen on
#define BACKLOG 10      // Max number of pending connections
#define MAX_CLIENTS 10  // Max number of active connections
#define MAX_REQUEST_SIZE (1024 * 1024) // 1 MB
#define MAX_HEADERS_SIZE (16 * 1024) // 16 KB
#define MAX_HEADERS_COUNT 100
#define MAX_HEADER_LINE 2048
#define MAX_FILE_SIZE (4 * 1024 * 1024) // 4 MB

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

typedef struct Request {
    int headers_len;
    int body_len;

    char method[8];
    char path[1024];
    char version[16];

    int content_len;
    int connection_close; // 1 = close, 0 = keep-alive

    char* body;
} request_t;

typedef struct Response {
    char* data;
    int len;
    int sent;

    int status_code;
    int connection_close; // 1 = close, 0 = keep-alive
} response_t;

typedef struct Client {
    client_state_t state;
    int fd;
    struct sockaddr_in addr;
    long last_activity_ms;
    int peer_closed;
    http_status_t error_code;

    char* buffer;
    int buffer_len;
    int buffer_cap;

    request_t request;
    int headers_done;
    int headers_len;

    response_t response;
} client_t;

typedef struct pollfd pollfd_t;

long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000L) + (ts.tv_nsec / 1000000L);
}

// ----------------------------------------------
// Signal handlers
// ----------------------------------------------

/*
 * Global flag for graceful shutdown.
 *
 * volatile: prevents compiler from caching the value in a register,
 *           ensuring the signal handler's write is visible.
 * sig_atomic_t: guarantees atomic read/write operations,
 *                making it safe to use in signal handlers.
 */
volatile sig_atomic_t keep_running = 1;

/*
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
    /*
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
        perror("sigaction failed: SIGINT | SIGTERM");
        exit(EXIT_FAILURE);
    }

    /*
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
        perror("sigaction failed: SIGPIPE");
        exit(EXIT_FAILURE);
    }
}

// ----------------------------------------------
// Server setup
// ----------------------------------------------

int setup_server() {
    /*
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
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    /*
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
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Socket created: %d\n", server_fd);

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    /*
     * htons() converts 16-bit values to network byte order.
     * htonl() converts 32-bit values to network byte order.
     */
    address.sin_family = AF_INET;                       // IPv4
    address.sin_port = htons(PORT);                     // Convert port to network byte order
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // localhost => 127.0.0.1
    // address.sin_addr.s_addr = htonl(INADDR_ANY);     // all available network interfaces => 0.0.0.0

    /*
     * bind(socket, address, address_length)
     *
     * We cast sockaddr_in* to sockaddr* because the API is generic.
     */
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    /*
     * STEP 4: Start listening for incoming connections
     *
     * listen(socket, backlog)
     *
     * backlog:
     *   Maximum number of pending connections in the queue
     */
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server is listening on port %d...\n", PORT);

    return server_fd;
}

// ----------------------------------------------
// Main loop helpers
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

/**
 * Free allocated memory for client
 */
void free_client(client_t* client) {
    if (client->buffer != NULL) {
        free(client->buffer);
        client->buffer = NULL;
        client->request.body = NULL;
    }
    if (client->response.data != NULL) {
        free(client->response.data);
        client->response.data = NULL;
    }
}

/**
 * Close client helper: close client socket, update sockets and clients arrays
 */
void close_client(pollfd_t* pfds, client_t* clients, int* nfds, int i) {
    close(pfds[i].fd);
    free_client(clients + i);

    pfds[i] = pfds[(*nfds) - 1];
    clients[i] = clients[(*nfds) - 1];

    (*nfds)--;
}

void set_client_error(client_t* client, http_status_t status_code) {
    client->error_code = status_code;
    client->state = STATE_HANDLING;
}

// ----------------------------------------------
// Read request
// ----------------------------------------------

/**
 * Parse request line, headers, populate `client->request`
 */
int parse_request_headers(client_t* client) {
    char* buf = client->buffer;

    /**
     * Parse request line
     */
    char* line_end = strstr(buf, "\r\n");
    if (!line_end) return 0;

    *line_end = '\0';

    if (sscanf(buf,
        "%7s %1023s %15s",
        client->request.method,
        client->request.path,
        client->request.version
    ) != 3) return 0;

    *line_end = '\r';

    if (strcmp(client->request.version, "HTTP/1.1") == 0) {
        client->request.connection_close = 0;
    }
    else {
        client->request.connection_close = 1;
    }

    /**
     * Parse headers
     */
    char* p = line_end + 2;
    char* headers_end = client->buffer + client->headers_len;

    client->request.content_len = 0;

    int headers_count = 0;

    while (p < headers_end - 2) {
        char* next = strstr(p, "\r\n");
        if (!next) return 0;

        if (next == p) break; // empty line

        headers_count++;
        if (headers_count > MAX_HEADERS_COUNT) return 0;

        *next = '\0';

        char key[MAX_HEADER_LINE];
        char value[MAX_HEADER_LINE];

        if (sscanf(p, "%[^:]: %2047[^\r\n]", key, value) == 2) {

            if (strcasecmp(key, "content-length") == 0) {
                char* end;
                long len = strtol(value, &end, 10);
                if (*end != '\0' || len < 0 || len > MAX_REQUEST_SIZE) return 0;
                client->request.content_len = (int)len;
            }
            else if (strcasecmp(key, "connection") == 0) {
                if (strcasecmp(value, "keep-alive") == 0) {
                    client->request.connection_close = 0;
                }
                else if (strcasecmp(value, "close") == 0) {
                    client->request.connection_close = 1;
                }
            }
            else if (strcasecmp(key, "transfer-encoding") == 0) {
                return -1; // not supported
            }
        }

        *next = '\r';
        p = next + 2;
    }

    client->request.body_len = client->request.content_len;

    return 1;
}

void reset_client_for_next_request(client_t* client) {
    int remaining = client->buffer_len - client->request.headers_len - client->request.content_len;
    if (remaining < 0) remaining = 0;

    // Shift buffer
    if (remaining > 0) {
        memmove(
            client->buffer,
            client->request.body + client->request.content_len,
            remaining
        );
    }
    client->buffer_len = remaining;
    client->buffer[client->buffer_len] = '\0';

    // Reset parsing state
    client->headers_done = 0;
    client->headers_len = 0;

    memset(&client->request, 0, sizeof(request_t));
    client->request.connection_close = 1;

    // Reset response
    if (client->response.data) {
        free(client->response.data);
        client->response.data = NULL;
    }

    memset(&client->response, 0, sizeof(response_t));

    client->last_activity_ms = now_ms();
    client->state = STATE_READING;
    client->error_code = 0;
}

/**
 * Read request from client socket
 * handles partial reads and interrupts
 */
void handle_read(client_t* client) {
    while (1) {
        /**
         * grow buffer if needed
         */
        if (client->buffer_len + 1 >= client->buffer_cap) {
            if (client->buffer_cap > MAX_REQUEST_SIZE / 2) return set_client_error(client, HTTP_400_BAD_REQUEST);
            client->buffer_cap *= 2;
            int body_offset = -1;
            if (client->request.body != NULL) body_offset = client->request.body - client->buffer;
            char* tmp = realloc(client->buffer, client->buffer_cap);
            if (tmp == NULL) return set_client_error(client, HTTP_500_INTERNAL_ERROR);
            client->buffer = tmp;
            if (body_offset >= 0) client->request.body = client->buffer + body_offset;
        }

        /**
         * read next bytes, handle errors
         */
        char* write_pos = client->buffer + client->buffer_len;
        size_t read_bytes = client->buffer_cap - client->buffer_len - 1; // room left in buffer

        // cap read to expected content len
        if (client->headers_done && client->request.content_len > 0) {
            // end of expected request = body start + declared content length
            char* want_end = client->request.body + client->request.content_len;

            if (want_end > write_pos) {
                size_t remaining = want_end - write_pos;
                if (remaining < read_bytes) read_bytes = remaining;
            }
            else {
                // already have everything; shouldn't normally reach here because
                // the previous iteration's check would have goto'd next_step,
                // but just in case.
                goto next_step;
            }
        }

        if (read_bytes == 0) {
            if (client->headers_done) goto next_step;
            else return set_client_error(client, HTTP_400_BAD_REQUEST);
        }

        ssize_t n = read(client->fd, write_pos, read_bytes);

        if (n < 0) {
            if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // If peer already closed, no more data will come
                if (client->peer_closed) return set_client_error(client, HTTP_400_BAD_REQUEST);
                break;
            }
            else set_client_error(client, HTTP_400_BAD_REQUEST);
        }

        /**
         * EOF
         */
        if (n == 0) {
            client->peer_closed = 1;

            // Case 1: no new request started -> clean close
            if (!client->headers_done && client->buffer_len == 0) {
                client->state = STATE_DONE;
                return;
            }

            // Case 2: partial request -> error
            if (!client->headers_done) return set_client_error(client, HTTP_400_BAD_REQUEST);

            // Case 3: headers done, check body
            // If no body expected OR already fully read -> OK
            int body_received = 0;
            if (client->request.body) {
                body_received = (client->buffer + client->buffer_len) - client->request.body;
            }
            if (body_received >= client->request.content_len) goto next_step;

            return set_client_error(client, HTTP_400_BAD_REQUEST);
        };

        client->buffer_len += n;
        client->buffer[client->buffer_len] = '\0';
        client->last_activity_ms = now_ms();

        if (!client->headers_done && client->buffer_len > MAX_HEADERS_SIZE) {
            fprintf(stderr, "Headers too large\n");
            return set_client_error(client, HTTP_400_BAD_REQUEST);
        }

        // check for headers end
        if (!client->headers_done) {
            char* headers_end = strstr(client->buffer, "\r\n\r\n");
            if (headers_end != NULL) {
                client->headers_done = 1;

                int headers_len = headers_end - client->buffer + 4;
                client->headers_len = headers_len;

                client->request.headers_len = headers_len;
                client->request.body = headers_end + 4;

                int ok = parse_request_headers(client);
                if (ok < 0) return set_client_error(client, HTTP_405_NOT_ALLOWED);
                if (ok < 1) return set_client_error(client, HTTP_400_BAD_REQUEST);
            }
        }

        if (client->headers_done) {
            int body_received = (client->buffer + client->buffer_len) - client->request.body;

            if (body_received >= client->request.content_len) goto next_step;
        }
    }
    return;

next_step: {
    printf(
        "---\n"
        "%.*s\n"
        "---\n",
        client->buffer_len,
        client->buffer
    );

    client->state = STATE_HANDLING;
    return;
    }
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
    int connection_close
) {
    printf("> build: status=%d, body_len=%d\n", status_code, body_len);

    int header_cap = 1024;

    int total_cap = header_cap + body_len;

    res->data = malloc(total_cap);
    if (!res->data) return 0;

    printf("> build: data allocated");

    const char* conn = connection_close ? "close" : "keep-alive";

    int header_len = snprintf(
        res->data,
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
        conn
    );

    printf("> build: header_len=%d\n", header_len);

    if (header_len < 0 || header_len >= header_cap) {
        free(res->data);
        return 0;
    }

    if (body_len > 0 && body != NULL) {
        memcpy(res->data + header_len, body, body_len);
    }

    res->len = header_len + body_len;
    res->sent = 0;
    res->connection_close = connection_close;

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
    int body_len = client->request.content_len;
    const char* body = client->request.body;

    if (!body) body_len = 0;

    if (!response_build(
        &client->response,
        HTTP_200_OK,
        "OK",
        "text/plain",
        body,
        body_len,
        client->request.connection_close
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
            int n = snprintf(out, cap, "%s", path);
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
    printf("%s\n", with_index);
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
        client->request.connection_close
    )) set_client_error(client, HTTP_500_INTERNAL_ERROR);

    free(file_data);
    return;
}

// ----------------------------------------------
// Handle request
// ----------------------------------------------

void handle_request(client_t* client, server_config_t* config) {
    if (client->error_code == 0) {
        if (config->mode == MODE_ECHO) {
            create_echo_response(client);
        }
        else {
            create_fs_response(client, config);
        }
    }

    if (client->error_code != 0 && (
        client->error_code < 100 || client->error_code > 599
        )) {
        fprintf(stderr, "Invalid HTTP status: %d\n", client->error_code);
        set_client_error(client, HTTP_500_INTERNAL_ERROR);
    }

    if (client->error_code != 0) {
        if (!create_error_response(client, client->error_code)) {
            client->state = STATE_ERROR;
            return;
        }
    }

    client->state = STATE_WRITING;
}

// ----------------------------------------------
// Send response
// ----------------------------------------------

/**
 * Write response to client socket
 * handles partial writes and interrupts
 */
void handle_write(client_t* client) {
    response_t* res = &client->response;

    printf(">> write: response len = %d, sent = %d\n", res->len, res->sent);

    printf(">> write: response:\n%.*s\n", res->len - res->sent, res->data + res->sent);

    while (res->sent < res->len) {
        printf(">> write: len = %d, sent = %d\n", res->len, res->sent);

        // write next bytes
        ssize_t n = write(
            client->fd,
            res->data + res->sent,
            res->len - res->sent
        );

        printf(">> write: write() => %zu\n", n);

        if (n <= 0) {
            if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            else {
                client->state = STATE_ERROR;
                return;
            }
        }

        res->sent += n;
        client->last_activity_ms = now_ms();
    }

    if (res->connection_close || client->peer_closed) {
        client->state = STATE_DONE;
    }
    else {
        reset_client_for_next_request(client);
    }
}

// ----------------------------------------------
// Main loop
// ----------------------------------------------

void init_server_event_loop(server_config_t* config) {
    setup_signal_handlers();

    int server_fd = setup_server();

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
            if (errno != EINTR) perror("poll");
            break;
        }

        /**
         * close timed-out connections
         */
        long now2 = now_ms();

        for (int i = 1; i < nfds; i++) {
            long idle = now2 - clients[i].last_activity_ms;
            if (idle >= config->keep_alive_timeout_ms) {
                printf("client timeout\n");
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

                printf("new connection\n");
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
                    if (errno != EINTR) perror("accept");
                    continue; // don't kill server, go to next connection
                }

                if (set_nonblocking(client_fd) < 0) {
                    perror("fcntl failed");
                    close(client_fd);
                    continue;
                }

                if (nfds >= MAX_CLIENTS + 1) {
                    printf("Too many clients, dropping connection\n");
                    close(client_fd);
                    continue;
                }

                int buffer_cap = 4096;
                char* buffer = malloc(sizeof(char) * buffer_cap);
                if (buffer == NULL) {
                    printf("Failed to allocate request buffer\n");
                    close(client_fd);
                    continue;
                }

                pfds[nfds].fd = client_fd;

                memset(&clients[nfds], 0, sizeof(client_t));

                clients[nfds].state = STATE_READING;
                clients[nfds].fd = client_fd;
                clients[nfds].addr = client_addr;
                clients[nfds].last_activity_ms = now_ms();
                clients[nfds].peer_closed = 0;
                clients[nfds].error_code = 0;

                clients[nfds].buffer = buffer;
                clients[nfds].buffer_len = 0;
                clients[nfds].buffer_cap = buffer_cap;

                memset(&clients[nfds].request, 0, sizeof(request_t));
                clients[nfds].request.connection_close = 0; // default HTTP/1.1 behavior

                clients[nfds].headers_done = 0;
                clients[nfds].headers_len = 0;

                memset(&clients[nfds].response, 0, sizeof(response_t));

                nfds++;

                continue;
            }

            /**
             * Socket error
             */
            if (pfds[i].revents & (POLLERR | POLLNVAL)) {
                fprintf(stderr, "socket error: ");
                if (pfds[i].revents & POLLERR) fprintf(stderr, "POLLERR\n");
                else  fprintf(stderr, "POLLNVAL\n");
                close_client(pfds, clients, &nfds, i);
                i--;
                continue;
            }

            /**
             * Client read/write ready
             *
             * After accept
             *  - read from client_fd
             *  - write to client_fd
             *  - close client_fd when done
             */

            client_t* client = &clients[i];

            printf("client ready, state = %d\n", client->state);

            if (client->state == STATE_READING && (pfds[i].revents & POLLIN)) {
                printf("client read\n");
                handle_read(client);
            }

            if (client->state == STATE_HANDLING) {
                printf("client handling\n");
                handle_request(client, config);
            }

            if (client->state == STATE_WRITING && (pfds[i].revents & POLLOUT)) {
                printf("client write\n");
                handle_write(client);
            }

            if (client->state == STATE_READING && client->buffer_len > 0 && !client->peer_closed) {
                // try to parse next request immediately (pipeline)
                handle_read(client);
            }

            if (client->state == STATE_READING && client->peer_closed && !(pfds[i].revents & POLLIN)) {
                // no more data will come
                printf("peer closed without full request\n");
                close_client(pfds, clients, &nfds, i);
                i--;
                continue;
            }

            if (client->state == STATE_DONE || client->state == STATE_ERROR) {
                if (client->state == STATE_DONE) {
                    printf("request done\n");
                }
                else {
                    fprintf(stderr, "request error\n");
                }
                close_client(pfds, clients, &nfds, i);
                i--;
                continue;
            }
        }
    }

    /*
     * Cleanup
     *
     * Close all open sockets (server and client sockets) and print shutdown message.
     * This code is now reachable thanks to the graceful shutdown
     * mechanism (signal_handler + poll() + keep_running).
     */
    printf("\nShutting down server...\n");
    for (int i = 0; i < nfds; i++) {
        close(pfds[i].fd);
        free_client(clients + i);
    }
    printf("Server shutdown.\n");
}

// ----------------------------------------------
// Entrypoint
// ----------------------------------------------

int main(int argc, char** argv) {
    server_config_t config = { 0 };

    if (argc < 2) {
        fprintf(stderr, "Usage: %s echo | fs <dir>\n", argv[0]);
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

    config.keep_alive_timeout_ms = 5000;

    init_server_event_loop(&config);
    return 0;
}
