#ifndef NETWORK_H
#define NETWORK_H

#include <stddef.h> // For size_t

// Forward declare app_state_t to avoid full include if only pointers are needed
// However, since app_state_t is used directly in some function signatures
// (though often via pointer), including peer.h is simpler here.
#include "peer.h"   // Provides app_state_t definition

/**
 * @brief Retrieves the primary non-loopback IPv4 address of the local machine.
 * @param buffer Buffer to store the IP address string.
 * @param size Size of the buffer.
 * @return 0 on success, -1 on failure.
 */
int get_local_ip(char *buffer, size_t size);

/**
 * @brief Sets receive and send timeouts for a given socket.
 * @param socket The socket file descriptor.
 * @param seconds The timeout duration in seconds.
 */
void set_socket_timeout(int socket, int seconds);

/**
 * @brief Initializes the TCP listening socket for incoming peer connections.
 * Binds to the TCP port and starts listening.
 * @param state Pointer to the application state.
 * @return 0 on success, -1 on failure.
 */
int init_listener(app_state_t *state);

/**
 * @brief Sends a formatted message to a specific peer via TCP.
 * Creates a temporary connection for sending the message.
 * @param ip The IP address of the recipient peer.
 * @param message The content of the message.
 * @param msg_type The type of the message (e.g., MSG_TEXT, MSG_QUIT).
 * @param sender_username The username of the peer sending the message.
 * @return 0 on success, -1 on failure.
 */
int send_message(const char *ip, const char *message, const char *msg_type, const char *sender_username);

/**
 * @brief The main function for the TCP listener thread.
 * Accepts incoming connections, reads messages, and processes them.
 * @param arg Pointer to the application state (app_state_t*).
 * @return NULL.
 */
void *listener_thread(void *arg);

#endif // NETWORK_H