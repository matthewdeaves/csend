// Include guard: Ensures the contents of this file are processed only once
// during compilation, even if the file is included multiple times.
#ifndef MESSAGING_H // If MESSAGING_H is not defined...
#define MESSAGING_H // ...define MESSAGING_H.

// Include the peer header file. This is necessary because the functions declared
// below use the `app_state_t` type, which is defined in `peer.h`.
// The listener initialization and thread functions require access to the shared
// application state.
#include "peer.h"   // Provides app_state_t definition and related constants (like PORT_TCP).

// --- Function Declarations ---
// These declarations inform the compiler about functions defined in messaging.c,
// allowing other source files (.c files) to call these TCP messaging-related functions.

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
 *          2. Sets connection and send timeouts (using functions from network.h/network.c).
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

#endif // End of the include guard MESSAGING_H