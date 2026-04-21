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

typedef enum ClientState {
    STATE_READING,
    STATE_HANDLING,
    STATE_WRITING,
    STATE_DONE,
    STATE_ERROR
} client_state_t;

typedef struct Client {
    client_state_t state;
    int fd;
    struct sockaddr_in addr;

    char* buffer;
    int buffer_len;
    int buffer_cap;

    char* body;

    int headers_done;
    int has_content_len;
    int content_len;

    char* response;
    int response_len;
    int response_sent;
} client_t;

typedef struct Request {
    int headers_len;
    int body_len;
    char method[8];
    char path[1024];
    char* headers;
    char* body;
} request_t;

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
    free(client->buffer);
    free(client->response);
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
 * Read request from client socket
 * handles partial reads and interrupts
 */
void handle_read(client_t* client) {
    while (1) {
        // grow buffer if needed
        if (client->buffer_len + 1 > client->buffer_cap) {
            if (client->buffer_cap * 2 > MAX_REQUEST_SIZE) {
                client->state = STATE_ERROR;
                return;
            }
            client->buffer_cap *= 2;
            char* tmp = realloc(client->buffer, client->buffer_cap);
            if (tmp == NULL) {
                client->state = STATE_ERROR;
                return;
            }
            client->buffer = tmp;
        }

        // read next bytes
        size_t read_bytes = client->buffer_cap - client->buffer_len;
        if (client->has_content_len) {
            read_bytes = (size_t)(client->body + client->content_len) - (size_t)client->buffer_len;
        }
        size_t n = read(
            client->fd,
            client->buffer + client->buffer_len,
            read_bytes
        );

        if (n < 0) {
            if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else {
                client->state = STATE_ERROR;
                return;
            }
        }

        if (n == 0) {
            client->state = STATE_HANDLING;

            char* buf = malloc(sizeof(char) * (client->buffer_len + 1));
            if (buf != NULL) {
                memcpy(buf, client->buffer, client->buffer_len);
                buf[client->buffer_len] = '\0';
                printf(
                    "---\n"
                    "%s\n"
                    "---\n",
                    buf
                );
                free(buf);
            }

            return;
        }

        client->buffer_len += n;

        // check headers end
        if (!client->headers_done) {
            char* body = strstr(client->buffer, "\r\n\r\n");
            if (body != NULL) {
                client->headers_done = 1;
                client->body = body + 4;

                // Parse Content-Length
                char* cl = strstr(client->buffer, "Content-Length:");
                if (cl != NULL) {
                    sscanf(cl, "Content-Length: %d", &client->content_len);
                    if (client->content_len < 0 || client->content_len > MAX_REQUEST_SIZE) {
                        client->state = STATE_ERROR;
                        return;
                    };
                    client->has_content_len = 1;
                }
            }
        }
    }
}

// ----------------------------------------------
// Create response
// ----------------------------------------------

void handle_request(client_t* client) {
    /**
     * Create echo response
     */

    int body_len = (client->buffer + client->buffer_len) - client->body;

    if (body_len > INT_MAX - 4096) {
        client->state = STATE_ERROR;
        return;
    }

    int response_cap = 4096 + body_len;
    client->response = malloc(sizeof(char) * response_cap);

    if (client->response == NULL) {
        perror("allocate response");
        client->state = STATE_ERROR;
        return;
    }

    int response_length = snprintf(
        client->response,
        response_cap,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        body_len,
        client->body
    );

    if (response_length < 0 || response_length >= response_cap) {
        fprintf(stderr, "write response: sprintf failed or truncated\n");
        client->state = STATE_ERROR;
        return;
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

void init_server_event_loop() {
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
    pfds[0].events = POLLIN;

    int nfds = 1;  // number of active fds

    while (keep_running) {
        // server_fd → listening socket (stays open)
        // client_fd → new socket for each client

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

                if (nfds >= MAX_CLIENTS) {
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
                pfds[nfds].events = POLLIN;

                clients[nfds].state = STATE_READING;
                clients[nfds].fd = client_fd;
                clients[nfds].addr = client_addr;
                clients[nfds].buffer = buffer;
                clients[nfds].buffer_len = 0;
                clients[nfds].buffer_cap = buffer_cap;
                clients[nfds].body = NULL;
                clients[nfds].headers_done = 0;
                clients[nfds].has_content_len = 0;
                clients[nfds].content_len = 0;
                clients[nfds].response = NULL;
                clients[nfds].response_len = 0;
                clients[nfds].response_sent = 0;

                continue;
            }

            /**
             * Socket error
             */
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                printf("socket error\n");
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
            printf("client ready\n");

            client_t client = clients[i];

            if (client.state == STATE_READING && (pfds[i].revents & POLLIN)) {
                printf("client read\n");
                handle_read(&client);
                continue;
            }

            if (client.state == STATE_HANDLING) {
                printf("client handle\n");
                handle_request(&client);
                continue;
            }

            if (client.state == STATE_WRITING) {
                pfds[i].events = POLLOUT;
                if (pfds[i].revents & POLLOUT) {
                    printf("client write\n");
                    handle_write(&client);
                }
                continue;
            }

            if (client.state == STATE_DONE || client.state == STATE_ERROR) {
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
    printf("Shutting down server...\n");
    for (int i = 0; i < nfds; i++) {
        close(pfds[i].fd);
        free_client(clients + i);
    }
    printf("Server shutdown.\n");
}

int main(void) {
    init_server_event_loop();
    return 0;
}
