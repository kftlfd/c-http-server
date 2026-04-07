#include <stdio.h>      // printf(), perror()
#include <stdlib.h>     // exit(), EXIT_FAILURE
#include <string.h>     // memset()
#include <unistd.h>     // close()
#include <arpa/inet.h>  // htons(), htonl(), sockaddr_in, INADDR_ANY
#include <sys/socket.h> // socket(), bind(), listen(), setsockopt()
#include <signal.h>     // signal(), sigaction()
#include <errno.h>      // EINTR
#include <poll.h>       // poll()

#define PORT 8080       // Port the server will listen on
#define BACKLOG 10      // Max number of pending connections

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
    (void)signum;
    keep_running = 0;
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
    if (server_fd == -1) {
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

    // Zero out the structure to avoid garbage values
    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;          // IPv4
    address.sin_port = htons(PORT);        // Convert port to network byte order

    /*
     * INADDR_LOOPBACK:
     *   Bind to localhost (127.0.0.1)
     *
     * INADDR_ANY:
     *   Bind to all available network interfaces
     *   (localhost, LAN IP, etc.)
     *
     * htonl() converts 32-bit values to network byte order.
     */
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

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
     */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /*
     * At this point, the server is ready to accept connections.
     * Next step would be:
     *   accept()
     * to handle incoming clients.
     */

    while (keep_running) {
        // server_fd → listening socket (stays open)
        // client_fd → new socket for each client

        /*
         * Use poll() before accept() to allow signal interruption.
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
        struct pollfd pfd = {
            .fd = server_fd,
            .events = POLLIN,
        };

        int ret = poll(&pfd, 1, -1);
        if (ret < 0) {
            /*
             * EINTR: A signal interrupted the poll() call.
             * This happens when SIGINT/SIGTERM is received.
             * Break out of the loop to allow graceful shutdown.
             */
            if (errno == EINTR) {
                break;
            }
            perror("poll");
            continue;
        }

        /*
         * What accept() does
         *  1. Takes one connection from the queue
         *  2. Creates a new socket (client_fd)
         *  3. Returns client address info (optional)
         *
         * Important behavior: Blocking call by default → waits until a client connects
         */
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd,
            (struct sockaddr*)&client_addr,
            &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue; // don't kill server, just try again, go to next connection
        }

        /*
         * After accept
         *  - read from client_fd
         *  - write to client_fd
         *  - close client_fd when done
         */

         /*
          * Read request into buffer
          */
        char buffer[4096];
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read < 0) {
            perror("read");
            close(client_fd);
            continue;
        }
        buffer[bytes_read] = '\0';

        printf("\nReceived request:\n---\n%s\n---\n", buffer);

        /*
         * Parse request line
         */
        char method[8];
        char path[1024];
        memset(method, 0, sizeof(method));
        memset(path, 0, sizeof(path));
        sscanf(buffer, "%7s %1023s", method, path);
        printf("Method: %s\n", method);
        printf("Path: %s\n", path);

        char* body = buffer;
        char* body_start = strstr(buffer, "\r\n\r\n");
        if (body_start) {
            body = body_start + 4;
        }

        /*
         * Create echo response
         */
        char response[8192];
        int body_length = strlen(body);
        int response_length = snprintf(
            response,
            sizeof(response),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            body_length,
            body
        );

        /*
         * Send response
         */
        ssize_t bytes_sent = write(client_fd, response, response_length);
        if (bytes_sent < 0) {
            perror("write failed");
        }

        /*
         * Finish response, close connection
         */
        close(client_fd);
    }

    /*
     * Cleanup
     *
     * Close the server socket and print shutdown message.
     * This code is now reachable thanks to the graceful shutdown
     * mechanism (signal_handler + poll() + keep_running).
     */
    close(server_fd);
    printf("\nServer shutdown.\n");

    return 0;
}
