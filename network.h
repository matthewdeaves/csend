// Include guard: Ensures the contents of this file are processed only once
// during compilation, even if the file is included multiple times.
#ifndef NETWORK_H // If NETWORK_H is not defined...
#define NETWORK_H // ...define NETWORK_H.

// Include standard definitions, specifically for the size_t type,
// which is used as the type for size parameters (like buffer sizes).
#include <stddef.h> // For size_t

// Include the peer header file. This is necessary because the functions declared
// below use the `app_state_t` type, which is defined in `peer.h`.
// The comment about forward declaration acknowledges that sometimes only `struct app_state_t;`
// would be needed, but here the full definition is required because function signatures
// use `app_state_t*`, necessitating knowledge of the struct's existence and type.
#include "peer.h"   // Provides app_state_t definition and related constants (like PORT_TCP).

// --- Function Declarations ---
// These declarations inform the compiler about functions defined in network.c,
// allowing other source files (.c files) to call these network-related functions.

/**
 * @brief Retrieves the primary non-loopback IPv4 address of the local machine.
 * @details This function attempts to find an IPv4 address associated with a network
 *          interface on the host machine, excluding the loopback address (127.0.0.1).
 *          It's useful for identifying the machine's address on the local network.
 * @param buffer A pointer to a character buffer where the IP address string will be stored
 *               if found. The buffer must be allocated by the caller.
 * @param size The size (in bytes) of the provided `buffer`. This prevents buffer overflows.
 * @return 0 on success, indicating a suitable IP address was found and copied to `buffer`.
 * @return -1 on failure, indicating no suitable IP address was found or an error occurred
 *         (e.g., during the system call to get interface addresses).
 */
int get_local_ip(char *buffer, size_t size);

/**
 * @brief Sets receive (`SO_RCVTIMEO`) and send (`SO_SNDTIMEO`) timeouts for a given socket.
 * @details Configures the specified socket so that blocking send and receive operations
 *          will return with an error (`EAGAIN` or `EWOULDBLOCK`) if they do not complete
 *          within the specified time limit. This prevents indefinite blocking.
 * @param socket The file descriptor of the socket for which to set the timeouts.
 * @param seconds The timeout duration in whole seconds.
 */
void set_socket_timeout(int socket, int seconds);

/**
 * @brief Initializes the main TCP listening socket for the application.
 * @details Creates a TCP socket, configures it for reuse (`SO_REUSEADDR`), binds it
 *          to listen on all available network interfaces (`INADDR_ANY`) on the designated
 *          TCP port (`PORT_TCP`), and puts it into listening mode to accept incoming connections.
 * @param state A pointer to the application's state structure (`app_state_t`), where the
 *              file descriptor of the created listening socket will be stored (`state->tcp_socket`).
 * @return 0 on success, indicating the listener socket is ready.
 * @return -1 on failure, indicating an error occurred during socket creation, configuration,
 *         binding, or listening. The `state->tcp_socket` will be set to -1 on failure.
 */
int init_listener(app_state_t *state);

/**
 * @brief Sends a formatted message to a specific peer using a *temporary* TCP connection.
 * @details This function handles the entire process of sending a single message:
 *          1. Creates a new TCP socket.
 *          2. Sets connection and send timeouts.
 *          3. Connects to the specified peer's IP address and TCP port (`PORT_TCP`).
 *          4. Formats the message using `format_message` (including type, sender, content).
 *          5. Sends the formatted message.
 *          6. Closes the connection immediately after sending.
 * @param ip A string containing the IPv4 address of the recipient peer.
 * @param message A string containing the main content/payload of the message.
 * @param msg_type A string indicating the type of the message (e.g., "TEXT", "QUIT"),
 *                 used by the protocol formatting.
 * @param sender_username The username of the peer sending this message, included in the
 *                        formatted message.
 * @return 0 on success, indicating the message was likely sent (TCP provides some reliability,
 *         but delivery isn't guaranteed at this layer if the connection drops later).
 * @return -1 on failure at any step (socket creation, connection, formatting, sending).
 */
int send_message(const char *ip, const char *message, const char *msg_type, const char *sender_username);

/**
 * @brief The main function designed to run in a separate thread for handling incoming TCP connections.
 * @details This function contains the loop that waits for and processes connections on the
 *          listening socket created by `init_listener`. It uses `select` for non-blocking waits,
 *          `accept`s new connections, reads the message data, parses it, updates peer status,
 *          handles different message types (like text or quit notifications), and closes the
 *          connection for each message.
 * @param arg A void pointer, expected to be cast to `app_state_t*` within the function,
 *            providing access to the shared application state (listening socket, peer list, etc.).
 * @return NULL. Standard practice for Pthread functions; the return value is typically unused.
 */
void *listener_thread(void *arg);

#endif // End of the include guard NETWORK_H