// FILE: ./posix/messaging.c
// Include the header file for this module
#include "messaging.h"

// Include headers for other project modules used
#include "network.h"     // For set_socket_timeout()
#include "../shared/protocol.h"   // For format_message(), parse_message(), MSG_* constants
#include "logging.h"       // For log_message()
// Note: peer.h is already included via messaging.h

// --- System Headers ---
#include <stdio.h>       // For perror
#include <string.h>      // For memset, strlen, strcmp
#include <unistd.h>      // For close, read
#include <errno.h>       // For errno, EINTR
#include <pthread.h>     // For pthread_mutex_lock/unlock used in listener_thread

// --- Network/Socket Headers ---
#include <sys/socket.h>  // Core socket functions and structures
#include <netinet/in.h>  // For sockaddr_in, htons, INADDR_ANY
#include <arpa/inet.h>   // For inet_pton, inet_ntop
#include <sys/select.h>  // For select(), fd_set macros
#include <sys/time.h>    // For struct timeval (used by select and set_socket_timeout)

/**
 * @brief Initializes the TCP listening socket for incoming peer connections.
 * @details Creates a TCP socket (`SOCK_STREAM`), sets the `SO_REUSEADDR` option,
 *          binds the socket to listen on all local interfaces (`INADDR_ANY`) on the
 *          predefined `PORT_TCP`, and puts the socket into listening mode using `listen`.
 * @param state Pointer to the application's shared state structure, where the
 *              TCP socket file descriptor will be stored.
 * @return 0 on successful initialization.
 * @return -1 on failure (e.g., socket creation, option setting, binding, or listening failed).
 *         Error details are printed via `perror`. The socket descriptor in the state
 *         is set to -1 on failure.
 */
int init_listener(app_state_t *state) {
    // Structure to hold the socket address information (IP address and port).
    struct sockaddr_in address;
    // Variable for setting socket options (1 = enable).
    int opt = 1;

    // Create a TCP socket.
    // AF_INET: Address family IPv4.
    // SOCK_STREAM: Socket type for stream-based communication (TCP).
    // 0: Protocol (IPPROTO_TCP, usually 0 for TCP).
    if ((state->tcp_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("TCP socket creation failed");
        return -1;
    }

    // Set socket option: SO_REUSEADDR.
    // Allows the socket to bind to the port even if it's in TIME_WAIT state.
    if (setsockopt(state->tcp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("TCP setsockopt(SO_REUSEADDR) failed");
        close(state->tcp_socket); // Clean up the created socket.
        state->tcp_socket = -1;   // Mark socket as invalid.
        return -1;
    }

    // Prepare the sockaddr_in structure for binding.
    address.sin_family = AF_INET; // IPv4.
    // Bind to all available local network interfaces.
    address.sin_addr.s_addr = INADDR_ANY;
    // Set the port number to listen on (convert to network byte order).
    address.sin_port = htons(PORT_TCP);

    // Bind the socket to the specified address and port.
    // This associates the socket with the port, allowing it to receive connections.
    if (bind(state->tcp_socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("TCP bind failed");
        close(state->tcp_socket); // Clean up.
        state->tcp_socket = -1;
        return -1;
    }

    // Put the socket into listening mode to accept incoming connections.
    // 10: The backlog queue size - maximum number of pending connections waiting to be accepted.
    if (listen(state->tcp_socket, 10) < 0) {
        perror("TCP listen failed");
        close(state->tcp_socket); // Clean up.
        state->tcp_socket = -1;
        return -1;
    }

    // Log successful initialization.
    log_message("TCP listener initialized on port %d", PORT_TCP);
    return 0; // Return success.
}

/**
 * @brief Sends a formatted message to a specific peer via TCP.
 * @details Establishes a *new* TCP connection to the target peer's IP address
 *          and port (`PORT_TCP`) for *each message*. It formats the message
 *          using the application protocol, sends it, and then immediately closes
 *          the connection. A short timeout is set for the connection attempt and send operation.
 * @param ip The IP address string of the recipient peer.
 * @param message The content/payload of the message to send.
 * @param msg_type The type of the message (e.g., `MSG_TEXT`, `MSG_QUIT`) from `protocol.h`.
 * @param sender_username The username of the peer sending the message.
 * @return 0 on success (message sent and connection closed).
 * @return -1 on failure (e.g., socket creation, address conversion, connection, formatting, or send failed).
 *         Error details are printed via `perror`.
 * @note This approach of creating a new connection per message is simple but inefficient
 *       for frequent communication. A persistent connection model might be better for performance.
 */
int send_message(const char *ip, const char *message, const char *msg_type, const char *sender_username) {
    // Socket file descriptor for this temporary connection.
    int sock;
    // Structure to hold the target server's address information.
    struct sockaddr_in server_addr;
    // Buffer to hold the formatted message before sending.
    char buffer[BUFFER_SIZE];
    // Buffer for local IP
    char local_ip[INET_ADDRSTRLEN];
    int formatted_len; // Store length returned by format_message

    // Create a new TCP socket for this outgoing connection.
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Outgoing TCP socket creation failed");
        return -1;
    }

    // Set a timeout (e.g., 5 seconds) for connect() and send() operations.
    set_socket_timeout(sock, 5);

    // Prepare the server address structure.
    // Initialize the structure to zero.
    memset(&server_addr, 0, sizeof(server_addr));
    // Set address family to IPv4.
    server_addr.sin_family = AF_INET;
    // Set the target port (where the recipient's listener thread is running).
    server_addr.sin_port = htons(PORT_TCP);

    // Convert the human-readable IP address string to the binary format needed by sockaddr_in.
    // inet_pton: "presentation to network" conversion.
    // Returns 1 on success, 0 if the input string is not a valid IP, -1 on error.
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid target IP address format");
        close(sock); // Close the socket we created.
        return -1;
    }

    // Attempt to establish a TCP connection to the target peer.
    // connect() blocks until the connection succeeds, fails, or times out (due to set_socket_timeout).
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        // perror will indicate the reason (e.g., "Connection refused", "Connection timed out").
        perror("TCP connection failed");
        close(sock); // Close the socket.
        return -1;
    }

    // Get local IP before formatting
    if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
        log_message("Warning: send_message failed to get local IP. Using 'unknown'.");
        strcpy(local_ip, "unknown");
    }

    // Format the message payload using the application protocol.
    // This includes the type, sender (username@local_ip), local_ip and content.
    formatted_len = format_message(buffer, BUFFER_SIZE, msg_type, sender_username, local_ip, message);
    if (formatted_len <= 0) { // format_message returns 0 on error, >0 on success
        log_message("Error: Failed to format outgoing message (buffer too small?).");
        close(sock);
        return -1;
    }


    // Send the formatted message over the established TCP connection.
    // send() attempts to send the specified number of bytes.
    // 0: Flags (usually 0 for standard send).
    // Returns the number of bytes sent, or -1 on error.
    if (send(sock, buffer, formatted_len - 1, 0) < 0) { // Send actual data length
        perror("TCP send failed");
        close(sock); // Close the socket on send failure.
        return -1;
    }

    // Close the TCP connection immediately after sending the message.
    // This releases the socket resources.
    close(sock);
    return 0; // Return success.
}

/**
 * @brief Main function for the TCP listener thread.
 * @details This function runs in a dedicated thread and is responsible for handling
 *          incoming TCP connections from other peers. It uses `select` to wait for
 *          activity on the main listening socket (`state->tcp_socket`) without blocking
 *          indefinitely, allowing the thread to periodically check the `state->running` flag.
 *          When a connection arrives:
 *          1. It `accept`s the connection, creating a new socket for communication with that client.
 *          2. It reads the message data from the client socket.
 *          3. It parses the received message using `parse_message`.
 *          4. It adds/updates the peer in the list using `add_peer`.
 *          5. It handles the message based on its type (`MSG_TEXT`, `MSG_QUIT`).
 *          6. It closes the client socket after processing the message.
 *          The loop continues as long as `state->running` is true.
 * @param arg A void pointer argument, expected to be a pointer to the `app_state_t` structure.
 * @return NULL. Standard practice for POSIX thread functions.
 */
void *listener_thread(void *arg) {
    // Cast the void pointer argument back to the expected app_state_t pointer.
    app_state_t *state = (app_state_t *)arg;
    // Structure to store the connecting client's address information.
    struct sockaddr_in client_addr;
    // Variable to store the size of the client_addr structure (used by accept).
    // Needs to be socklen_t for accept(). Initializing here avoids potential issues.
    socklen_t addrlen = sizeof(client_addr);
    // File descriptor for the socket connected to a specific client (returned by accept).
    int client_sock;
    // Buffer to hold incoming message data read from the client socket.
    char buffer[BUFFER_SIZE];
    // Buffers to store parsed message components.
    char sender_ip[INET_ADDRSTRLEN];
    char sender_username[32];
    char msg_type[32];
    char content[BUFFER_SIZE];
    char sender_ip_from_payload[INET_ADDRSTRLEN]; // Buffer for IP extracted by parse_message
    // File descriptor set used with select() to monitor the listening socket.
    fd_set readfds;
    // Timeout structure for select().
    struct timeval timeout;
    // Variable to store the number of bytes read
    ssize_t bytes_read;

    log_message("Listener thread started");

    // Main loop: continues as long as the application is running.
    while (state->running) {
        // Prepare the file descriptor set for select().
        FD_ZERO(&readfds); // Clear the set.
        // Add the main listening socket to the set. We want to know when it's ready for reading (i.e., has an incoming connection).
        FD_SET(state->tcp_socket, &readfds);

        // Set the timeout for select().
        timeout.tv_sec = 1;  // Wait for 1 second.
        timeout.tv_usec = 0; // 0 microseconds.

        // Use select() to wait for activity on the listening socket or until the timeout expires.
        // state->tcp_socket + 1: Highest file descriptor number + 1 (required by select).
        // &readfds: Set of descriptors to check for read readiness.
        // NULL, NULL: Sets for write readiness and exceptions (not needed here).
        // &timeout: Maximum time to wait.
        // Returns the number of ready descriptors, 0 on timeout, -1 on error.
        int activity = select(state->tcp_socket + 1, &readfds, NULL, NULL, &timeout);

        // Check for errors from select().
        // EINTR means the call was interrupted by a signal (e.g., SIGINT), which is okay if we're shutting down.
        if (activity < 0 && errno != EINTR) {
            perror("Select error in listener thread");
            break; // Exit the loop on unrecoverable select error.
        }

        // Check if the application is still supposed to be running after select() returns.
        // This handles the case where a shutdown signal was received while select() was waiting.
        if (!state->running) {
            break; // Exit the loop if the running flag is now false.
        }

        // If select() timed out (activity == 0) or was interrupted (activity < 0, errno == EINTR),
        // just continue the loop to check the running flag and set up select() again.
        if (activity <= 0) {
            continue;
        }

        // If we reach here, it means activity > 0, and FD_ISSET(state->tcp_socket, &readfds) must be true
        // (since it was the only descriptor in the set). This indicates an incoming connection is pending.

        // Accept the incoming connection.
        // accept() blocks until a connection is present (but we know one is ready due to select).
        // It creates a *new* socket (client_sock) for communicating with this specific client.
        // The original listening socket (state->tcp_socket) remains open for further connections.
        // client_addr will be filled with the client's address information.
        if ((client_sock = accept(state->tcp_socket, (struct sockaddr *)&client_addr, &addrlen)) < 0) {
            // Check if accept was interrupted by a signal (EINTR), which might happen during shutdown.
            if (errno != EINTR) {
                perror("TCP accept failed");
                // Continue the loop even if accept fails for one connection, maybe the next will work.
            }
            continue;
        }

        // Get the client's IP address string from the client_addr structure.
        inet_ntop(AF_INET, &client_addr.sin_addr, sender_ip, INET_ADDRSTRLEN);

        // Read the message data from the newly accepted client socket.
        // Clear the buffer first.
        memset(buffer, 0, BUFFER_SIZE);
        // read() attempts to read up to BUFFER_SIZE bytes into the buffer.
        // Returns the number of bytes read, 0 if the client closed the connection, -1 on error.
        // Note: This assumes the entire message arrives in one chunk. For robust TCP,
        // you might need a loop to handle partial reads or messages larger than BUFFER_SIZE.
        bytes_read = read(client_sock, buffer, BUFFER_SIZE - 1); // Read BUFFER_SIZE-1 to ensure null termination possible

        if (bytes_read > 0) {
            // Null-terminate the received data to treat it as a string.
            // buffer[bytes_read] = '\0'; // Not strictly needed if parse_message handles length

            // Parse the received message using the application protocol.
            // Pass the actual number of bytes read.
            // Use sender_ip_from_payload as the output buffer for the IP within the message.
            if (parse_message(buffer, bytes_read, sender_ip_from_payload, sender_username, msg_type, content) == 0) {
                // Successfully parsed.

                // Add or update the peer in the application's list.
                // Use the IP address obtained from the connection (inet_ntop result - sender_ip).
                int add_result = add_peer(state, sender_ip, sender_username);
                if (add_result > 0) {
                    // Log if it was a newly discovered peer.
                    log_message("New peer connected via TCP: %s@%s", sender_username, sender_ip);
                } else if (add_result < 0) {
                    log_message("Peer list full, could not add %s@%s from TCP connection", sender_username, sender_ip);
                }
                // If add_result == 0, the peer was already known and updated.

                // Handle the message based on its type.
                if (strcmp(msg_type, MSG_TEXT) == 0) {
                    // Log incoming text messages.
                    log_message("Message from %s@%s: %s", sender_username, sender_ip, content);
                } else if (strcmp(msg_type, MSG_QUIT) == 0) {
                    // Handle quit notifications.
                    log_message("Peer %s@%s has sent QUIT notification", sender_username, sender_ip);

                    // Mark the peer as inactive in the list.
                    // Lock the mutex to ensure thread-safe access to the peers array.
                    pthread_mutex_lock(&state->peers_mutex);
                    for (int i = 0; i < MAX_PEERS; i++) {
                        // Find the peer by IP address.
                        if (state->peers[i].active && strcmp(state->peers[i].ip, sender_ip) == 0) {
                            // Mark as inactive. It will be pruned later or overwritten.
                            state->peers[i].active = 0;
                            log_message("Marked peer %s@%s as inactive.", state->peers[i].username, state->peers[i].ip);
                            break; // Found the peer, no need to continue loop.
                        }
                    }
                    // Unlock the mutex.
                    pthread_mutex_unlock(&state->peers_mutex);
                }
                // Other message types could be handled here if added later.

            } else {
                // Message parsing failed (e.g., bad magic number). Log the raw buffer content for debugging.
                log_message("Failed to parse TCP message from %s (%ld bytes)", sender_ip, bytes_read);
            }
        } else if (bytes_read == 0) {
            // read() returned 0, meaning the client closed the connection gracefully.
            log_message("Peer %s disconnected.", sender_ip);
        } else {
            // read() returned -1, indicating an error.
            perror("TCP read failed");
        }

        // Close the client-specific socket.
        // The listening socket (state->tcp_socket) remains open.
        close(client_sock);
    } // End of while(state->running) loop

    // Log when the thread is stopping.
    log_message("Listener thread stopped");
    return NULL; // Standard return for thread functions.
}