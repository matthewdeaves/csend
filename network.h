// Include guard: Ensures the contents of this file are processed only once
// during compilation, even if the file is included multiple times.
#ifndef NETWORK_H // If NETWORK_H is not defined...
#define NETWORK_H // ...define NETWORK_H.

// Include standard definitions, specifically for the size_t type,
// which is used as the type for size parameters (like buffer sizes).
#include <stddef.h> // For size_t

#include "peer.h" // Provides the definition of app_state_t

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

#endif // End of the include guard NETWORK_H