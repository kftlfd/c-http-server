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
#define MAX_CLIENTS 10 // Max number of active connections
#define MAX_REQUEST_SIZE (1024 * 1024) // 1 MB

typedef struct Request {
    int headers_len;
    int body_len;
    char method[8];
    char path[1024];
    char* headers;
    char* body;
} request_t;

void handle_client(int client_fd);
int read_request(int fd, request_t* req);
void free_req(request_t req);
int write_all(int fd, const char* buf, int len);

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

int main(void) {
    int server_fd;

    /*
     * STEP 1: Create a socket
     *
     * socket(domain, type, protocol)
     *
     * AF_INET     -> IPv4
     * SOCK_STREAM -> TCP (reliable, connection-based)
     * 0           -> automatically select protocol (TCP)
     */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
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

    /*
     * STEP 5: Set up signal handlers for graceful shutdown
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
        perror("sigaction failed");
        exit(EXIT_FAILURE);
    }

    /*
     * If client closes connection / crashed / reset the socket
     * during sending response with `write()` -> kernel may send SIGPIPE
     * by default it terminates the process immediately
     * ignore it instead
     */
    signal(SIGPIPE, SIG_IGN);

    /*
     * At this point, the server is ready to accept connections.
     * Next step would be:
     *   accept()
     * to handle incoming clients.
     */

     /*
      * poll() setup
      *
      * pollfd structure:
      *   fd      -> file descriptor
      *   events  -> what we want to watch (e.g., POLLIN)
      *   revents -> what actually happened
      */
    struct pollfd pollfds[MAX_CLIENTS + 1];

    // Index 0 is always the server socket
    pollfds[0].fd = server_fd;
    pollfds[0].events = POLLIN;

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
        if (poll(pollfds, nfds, -1) < 0) { // -1 = infinite timeout
            /*
             * EINTR: A signal interrupted the poll() call.
             * This happens when SIGINT/SIGTERM is received.
             * Break out of the loop to allow graceful shutdown.
             */
            if (errno == EINTR) {
                break;
            }
            perror("poll");
            break;
        }

        // Check all file descriptors
        for (int i = 0; i < nfds; i++) {
            if (pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(pollfds[i].fd);
                pollfds[i] = pollfds[nfds - 1];
                nfds--;
                i--;
                continue;
            }

            if (!(pollfds[i].revents & POLLIN)) {
                continue;
            }

            // New incoming connection
            if (pollfds[i].fd == server_fd) {
                /*
                 * What accept() does
                 *  1. Takes one connection from the queue
                 *  2. Creates a new socket (client_fd)
                 *  3. Returns client address info (optional)
                 *
                 * Important behavior: Blocking call by default → waits until a client connects
                 */
                struct sockaddr_in client_addr;
                memset(&client_addr, 0, sizeof(client_addr));
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd,
                    (struct sockaddr*)&client_addr,
                    &client_len);
                if (client_fd < 0) {
                    if (errno == EINTR) continue;
                    perror("accept");
                    continue; // don't kill server, just try again, go to next connection
                }

                /*
                 * Set non-blocking mode for socket
                 * fcntl = "file control", get/modify properties of file descriptor
                 * nonblock -> on read() and write()
                 */
                int flags = fcntl(client_fd, F_GETFL, 0);
                if (flags < 0 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                    perror("fcntl failed");
                    close(client_fd);
                    continue;
                }

                // Store socket to poll array
                if (nfds < MAX_CLIENTS) {
                    pollfds[nfds].fd = client_fd;
                    pollfds[nfds].events = POLLIN;
                    nfds++;
                }
                else {
                    printf("Too many clients, dropping request\n");
                    close(client_fd);
                }

                continue;
            }

            // Existing client sent data
            int client_fd = pollfds[i].fd;

            /*
             * After accept
             *  - read from client_fd
             *  - write to client_fd
             *  - close client_fd when done
             */

            handle_client(client_fd);

            close(client_fd);

            // Remove fd from poll list
            pollfds[i] = pollfds[nfds - 1];
            nfds--;
            i--;
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
        close(pollfds[i].fd);
    }
    printf("Server shutdown.\n");

    return 0;
}

void handle_client(int client_fd) {
    // Read request
    request_t req;
    memset(&req, 0, sizeof(req));
    if (read_request(client_fd, &req) < 0) {
        perror("read request");
        return;
    }

    printf(
        "---\n"
        "Received request: %s %s\n\n"
        "%s\n\n"
        "%s\n"
        "---\n",
        req.method, req.path,
        req.headers, req.body
    );

    // Create echo response
    if (req.body_len > INT_MAX - 4096) {
        free_req(req);
        return;
    }

    int response_cap = 4096 + req.body_len;
    char* response = malloc(sizeof(char) * response_cap);

    if (response == NULL) {
        perror("allocate response");
        free_req(req);
        return;
    }

    int response_length = snprintf(
        response,
        response_cap,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        req.body_len,
        req.body
    );
    if (response_length < 0 || response_length >= response_cap) {
        fprintf(stderr, "write response: sprintf failed or truncated\n");
        free(response);
        free_req(req);
        return;
    }

    // Send response
    if (write_all(client_fd, response, response_length) < 0) {
        perror("send response");
    }

    free(response);
    free_req(req);
}

/*
 * read request from socket (handles partial reads)
 */
int read_request(int fd, request_t* req) {
    if (req == NULL) {
        return -1;
    }
    /*
     * Read until:
     * - hheaders are complete ("\r\n\r\n")
     * - AND full body is read (via Content-Length)
     */

    int capacity = 4096;
    int total = 0;
    char* buffer = malloc(capacity);
    if (buffer == NULL) return -1;

    int headers_done = 0;
    int content_len = 0;
    int has_content_len = 0;

    char* headers_end = NULL;

    char* headers = NULL;
    int headers_len = 0;

    char* body = NULL;
    int body_len = 0;

    while (1) {
        if (total + 1 >= capacity) {
            capacity *= 2;
            char* tmp = realloc(buffer, capacity);
            if (tmp == NULL) goto err_cleanup;
            buffer = tmp;
        }

        ssize_t n = read(fd, buffer + total, capacity - total);

        if (n < 0) {
            if (errno == EINTR) continue;

            // No more data available right now
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;

            goto err_cleanup;
        }
        if (n == 0) break;

        total += n;
        if (total > MAX_REQUEST_SIZE) goto err_cleanup;

        buffer[total] = '\0';

        // Detect end of headers
        if (!headers_done) {
            headers_end = strstr(buffer, "\r\n\r\n");
            if (headers_end != NULL) {
                headers_done = 1;

                // Save headers
                headers_len = headers_end - buffer + 4;
                headers = malloc(sizeof(char) * headers_len);
                if (headers == NULL) goto err_cleanup;

                memcpy(headers, buffer, headers_len - 1);
                headers[headers_len - 1] = '\0';

                // Parse Content-Length
                char* cl = strstr(buffer, "Content-Length:");
                if (cl != NULL) {
                    has_content_len = 1;
                    if (sscanf(cl, "Content-Length: %d", &content_len) != 1) goto err_cleanup;
                    if (content_len < 0 || content_len > MAX_REQUEST_SIZE) goto err_cleanup;
                }
            }
        }

        if (headers_done) {
            body_len = total - (headers_end + 4 - buffer);

            if (body_len >= content_len) {
                break;
            }
        }
    }

    if (total <= 0 || headers_end == NULL) goto err_cleanup;

    // Parse request line
    memset(req->method, 0, sizeof(req->method));
    memset(req->path, 0, sizeof(req->path));
    if (sscanf(headers, "%7s %1023s", req->method, req->path) != 2) goto err_cleanup;

    // Save headers
    req->headers = headers;
    req->headers_len = headers_len;

    // Save body
    if (has_content_len) {
        if (body_len < 0 || body_len != content_len) goto err_cleanup;

        body = malloc(sizeof(char) * (body_len + 1));
        if (body == NULL) goto err_cleanup;

        memcpy(body, headers_end + 4, body_len);
        body[body_len] = '\0';

        req->body_len = body_len;
        req->body = body;
    }
    else if (body_len > 0) {
        goto err_cleanup;
    }

    free(buffer);
    return 1;

err_cleanup:
    free(buffer);
    free(headers);
    free(body);
    return -1;
}

void free_req(request_t req) {
    free(req.headers);
    free(req.body);
}

/*
 * write all bytes to socket (handles partial writes)
 */
int write_all(int fd, const char* buf, int len) {
    /*
     * write() may not send all bytes in one call (partial write).
     * Loop until all bytes are sent.
     */
    int total = 0;

    while (total < len) {
        ssize_t n = write(fd, buf + total, len - total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            return -1;
        }
        total += n;
    }

    return 0;
}
