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

#define PORT 8080       // Port the server will listen on
#define BACKLOG 10      // Max number of pending connections
#define MAX_CLIENTS 10  // Max number of active connections
#define MAX_REQUEST_SIZE (1024 * 1024) // 1 MB
#define MAX_HEADERS_SIZE (16 * 1024) // 16 KB
#define MAX_HEADER_LINE 2048

typedef enum {
    MODE_ECHO,
    MODE_FS
} server_mode_t;

typedef struct {
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

typedef struct Client {
    client_state_t state;
    int fd;
    struct sockaddr_in addr;

    char* buffer;
    int buffer_len;
    int buffer_cap;

    request_t request;
    int headers_done;
    int headers_len;
    int has_content_len;
    int content_len;

    char* response;
    int response_len;
    int response_sent;
} client_t;


typedef struct pollfd pollfd_t;

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
    if (client->response != NULL) {
        free(client->response);
        client->response = NULL;
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

// ----------------------------------------------
// Read request
// ----------------------------------------------

/**
 * Parse request line, headers, populate `client->request`
 */
int parse_request(client_t* client) {
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

    /**
     * Parse headers
     */
    char* p = line_end + 2;
    char* headers_end = client->buffer + client->headers_len;

    client->request.content_len = 0;
    client->request.connection_close = 1;

    while (p < headers_end - 2) {
        char* next = strstr(p, "\r\n");
        if (!next) return 0;

        if (next == p) break; // empty line

        *next = '\0';

        char key[MAX_HEADER_LINE];
        char value[MAX_HEADER_LINE];

        if (sscanf(p, "%[^:]: %2047[^\r\n]", key, value) == 2) {

            if (strcasecmp(key, "content-length") == 0) {
                int len = atoi(value);
                if (len < 0 || len > MAX_REQUEST_SIZE) return 0;
                client->request.content_len = len;
            }
            else if (strcasecmp(key, "connection") == 0) {
                if (strcasecmp(value, "keep-alive") == 0) {
                    client->request.connection_close = 0;
                }
                else {
                    client->request.connection_close = 1;
                }
            }

        }

        *next = '\r';
        p = next + 2;
    }

    client->request.body_len = client->request.content_len;

    return 1;
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
            if (client->buffer_cap > MAX_REQUEST_SIZE / 2) goto err_next;
            client->buffer_cap *= 2;
            int body_offset = -1;
            if (client->request.body != NULL) body_offset = client->request.body - client->buffer;
            char* tmp = realloc(client->buffer, client->buffer_cap);
            if (tmp == NULL) goto err_next;
            client->buffer = tmp;
            if (body_offset >= 0) client->request.body = client->buffer + body_offset;
        }

        /**
         * read next bytes, handle errors
         */
        char* write_pos = client->buffer + client->buffer_len;
        size_t read_bytes = client->buffer_cap - client->buffer_len - 1; // room left in buffer

        // cap read to expected content len
        if (client->headers_done && client->has_content_len) {
            // end of expected request = body start + declared content length
            char* want_end = client->request.body + client->content_len;

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
            else goto err_next;
        }

        ssize_t n = read(client->fd, write_pos, read_bytes);

        if (n < 0) {
            if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else goto err_next;
        }

        /**
         * finished reading
         */
        if (n == 0) {
            if (client->headers_done) goto next_step;
            else goto err_next;
        };

        client->buffer_len += n;
        client->buffer[client->buffer_len] = '\0';

        if (!client->headers_done && client->buffer_len > MAX_HEADERS_SIZE) {
            fprintf(stderr, "Headers too large\n");
            goto err_next;
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

                client->has_content_len = 0;
                client->content_len = 0;
            }
        }

        if (client->headers_done) {
            if (!parse_request(client)) goto err_next;

            int body_received = (client->buffer + client->buffer_len) - client->request.body;

            if (body_received >= client->request.content_len) goto next_step;
        }
    }
    return;

err_next: { client->state = STATE_ERROR; return; }

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
// Create response
// ----------------------------------------------

int create_echo_response(client_t* client) {
    if (client->request.content_len == 0 || client->request.body == NULL) {
        client->response = malloc(1024);
        if (client->response == NULL) return 1;

        client->response_len = snprintf(
            client->response,
            1024,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n",
            0
        );

        if (client->response_len < 0) {
            fprintf(stderr, "write response: sprintf failed or truncated\n");
            return 0;
        }
    }
    else {
        int body_len = client->request.content_len;

        if (body_len > INT_MAX - 4096) return 0;

        int response_cap = 4096 + body_len;
        client->response = malloc(sizeof(char) * response_cap);

        if (client->response == NULL) {
            perror("allocate response");
            return 0;
        }

        client->response_len = snprintf(
            client->response,
            response_cap,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            body_len,
            client->request.body
        );

        if (client->response_len < 0 || client->response_len >= response_cap) {
            fprintf(stderr, "write response: sprintf failed or truncated\n");
            return 0;
        }
    }
    return 1;
}

int create_fs_response(client_t* client, server_config_t* config) {
    client->response = malloc(1024);
    if (!client->response) return 0;

    char* response_body = malloc(1024);
    if (!response_body) return 0;

    int body_len = snprintf(response_body, 1024,
        "Not implemented\n"
        "Serving files from %s\n",
        config->fs_root
    );
    if (body_len < 1) { free(response_body); return 0; }

    int n = snprintf(client->response, 1024,
        "HTTP/1.1 501 Not Implemented\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        body_len,
        response_body
    );
    if (n < 1) { free(response_body); return 0; }

    client->response_len = n;
    free(response_body);
    return 1;
}

void handle_request(client_t* client, server_config_t* config) {
    int ok = 0;
    if (config->mode == MODE_ECHO) {
        ok = create_echo_response(client);
    }
    else {
        ok = create_fs_response(client, config);
    }

    if (ok) client->state = STATE_WRITING;
    else client->state = STATE_ERROR;
}

// ----------------------------------------------
// Send response
// ----------------------------------------------

/**
 * Write response to client socket
 * handles partial writes and interrupts
 */
void handle_write(client_t* client) {
    while (client->response_sent < client->response_len) {
        // write next bytes
        ssize_t n = write(
            client->fd,
            client->response + client->response_sent,
            client->response_len - client->response_sent
        );

        if (n <= 0) {
            if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            else {
                client->state = STATE_ERROR;
                return;
            }
        }

        client->response_sent += n;
    }

    client->state = STATE_DONE;
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

        /*
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
        if (poll(pfds, nfds, -1) < 0) { // -1 = infinite timeout
            /*
             * EINTR: A signal interrupted the poll() call.
             * This happens when SIGINT/SIGTERM is received.
             * Break out of the loop to allow graceful shutdown.
             */
            if (errno != EINTR) perror("poll");
            break;
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

                clients[nfds].state = STATE_READING;
                clients[nfds].fd = client_fd;
                clients[nfds].addr = client_addr;
                clients[nfds].buffer = buffer;
                clients[nfds].buffer_len = 0;
                clients[nfds].buffer_cap = buffer_cap;

                memset(&clients[nfds].request, 0, sizeof(request_t));
                clients[nfds].request.connection_close = 0; // default HTTP/1.1 behavior

                clients[nfds].headers_done = 0;
                clients[nfds].headers_len = 0;
                clients[nfds].has_content_len = 0;
                clients[nfds].content_len = 0;
                clients[nfds].response = NULL;
                clients[nfds].response_len = 0;
                clients[nfds].response_sent = 0;

                nfds++;

                continue;
            }

            /**
             * Socket error
             */
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fprintf(stderr, "socket error\n");
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

            if (client->state == STATE_DONE || client->state == STATE_ERROR) {
                if (client->state == STATE_DONE) {
                    printf("request done\n");
                }
                else {
                    printf("request error\n");
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
    }
    else {
        fprintf(stderr, "Unknown command\n");
        exit(1);
    }

    config.keep_alive_timeout_ms = 5000;

    init_server_event_loop(&config);
    return 0;
}
