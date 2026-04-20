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
#define MAX_CLIENTS 10 // Max number of active connections

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
            // Check that POLLIN event is received
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
                    perror("accept");
                    continue; // don't kill server, just try again, go to next connection
                }
                if (nfds < MAX_CLIENTS) {
                    pollfds[nfds].fd = client_fd;
                    pollfds[nfds].events = POLLIN;
                    nfds++;
                }
                else {
                    printf("Tpp many clients, dropping request\n");
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

             /*
              * Read request into buffer
              */
            char buffer[4096];
            ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
            if (bytes_read <= 0) {
                // Client closed connection or error
                perror("read");
                close(client_fd);
                // remove fd from array (swap with last)
                pollfds[i] = pollfds[nfds - 1];
                nfds--;
                i--; // re-check the current index (now contains the swapped fd)
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

            /*
             * Extract request body
             */
            char* body = strstr(buffer, "\r\n\r\n");
            if (body != NULL) {
                body += 4;
            }
            else {
                body = "";
            }
            int body_len = strlen(body);

            /*
             * Create echo response
             */
            char response[8192];
            int response_length = snprintf(
                response,
                sizeof(response),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: %d\r\n"
                "\r\n"
                "%s",
                body_len,
                body
            );

            /*
             * Send response
             *
             * write() may not send all bytes in one call (partial write).
             * Loop until all bytes are sent.
             */
            size_t total_sent = 0;
            while (total_sent < (size_t)response_length) {
                ssize_t bytes_sent = write(
                    client_fd,
                    response + total_sent,
                    response_length - total_sent
                );
                if (bytes_sent < 0) {
                    perror("write failed");
                    break;
                }
                total_sent += bytes_sent;
            }

            // Finish response, close connection
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
    printf("\nShutting down server...\n");
    for (int i = 0; i < nfds; i++) {
        close(pollfds[i].fd);
    }
    printf("\nServer shutdown.\n");

    return 0;
}
