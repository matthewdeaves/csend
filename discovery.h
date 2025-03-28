// Include guard: Prevents the header file from being included multiple times
// in the same compilation unit, which would cause redefinition errors.
#ifndef DISCOVERY_H // If DISCOVERY_H is not defined...
#define DISCOVERY_H // ...define DISCOVERY_H.

// Include necessary system headers for network programming types used in function signatures.
#include <sys/socket.h> // Provides definitions for socket functions and structures, including socklen_t.
#include <netinet/in.h> // Provides definitions for internet domain addresses (e.g., struct sockaddr_in).

// Include the peer header file. This is necessary because the functions declared
// below use the `app_state_t` type, which is defined in `peer.h`.
// While forward declaration (`struct app_state_t;`) could be used if only pointers
// to the struct were needed, `app_state_t` is used directly as a parameter type
// (via pointer) in these function signatures, making the full definition necessary.
#include "peer.h"       // Provides the definition of app_state_t and related constants (like PORT_UDP).

// --- Function Declarations ---
// These declarations tell the compiler about the existence, parameters, and return types
// of functions defined in discovery.c, allowing other parts of the program to call them.

/**
 * @brief Initializes the UDP socket required for peer discovery operations.
 * @details Sets up a UDP socket, enables broadcast capability, binds it to the
 *          predefined discovery port (`PORT_UDP`), and sets appropriate socket options
 *          (like `SO_REUSEADDR` and receive timeout). This socket will be used for
 *          both sending discovery broadcasts and receiving discovery messages/responses.
 * @param state A pointer to the main application state structure (`app_state_t`),
 *              which holds the file descriptor for the UDP socket once created.
 * @return 0 if the socket was initialized successfully.
 * @return -1 if any error occurred during socket creation, option setting, or binding.
 *         The specific error is typically printed to stderr via `perror` in the implementation.
 */
int init_discovery(app_state_t *state);

/**
 * @brief The main function designed to run in a separate thread for handling peer discovery.
 * @details This function contains the core logic for the discovery process. It runs a loop that:
 *          1. Periodically broadcasts discovery messages to the network.
 *          2. Listens for incoming UDP packets (discovery messages or responses from other peers).
 *          3. Processes received packets using `handle_discovery_message`.
 *          The loop continues as long as the application's running flag is set.
 * @param arg A void pointer, expected to be cast to `app_state_t*` within the function.
 *            This provides the thread with access to the shared application state.
 * @return NULL. This is standard practice for Pthread functions; the return value is often unused.
 */
void *discovery_thread(void *arg);

/**
 * @brief Sends a UDP broadcast message to discover other peers on the network.
 * @details Formats a discovery message according to the application protocol
 *          (including the sender's username and IP) and sends it to the network's
 *          broadcast address (`INADDR_BROADCAST`) on the discovery port (`PORT_UDP`).
 * @param state A pointer to the application state structure, used to access the
 *              UDP socket and the sender's username.
 * @return 0 if the broadcast message was sent successfully.
 * @return -1 if an error occurred during message formatting or sending (e.g., `sendto` failed).
 */
int broadcast_discovery(app_state_t *state);

/**
 * @brief Processes an incoming UDP packet received on the discovery socket.
 * @details Parses the packet's content using the application protocol. If the message
 *          is a valid `MSG_DISCOVERY` or `MSG_DISCOVERY_RESPONSE`, it extracts sender
 *          information (username, IP) and updates the application's peer list via `add_peer`.
 *          If it's a `MSG_DISCOVERY`, it also sends a `MSG_DISCOVERY_RESPONSE` back to the sender.
 * @param state Pointer to the application state structure (for accessing peer list, username, UDP socket).
 * @param buffer A pointer to the character buffer containing the raw data received from the UDP socket.
 * @param sender_ip A character buffer (provided by the caller) where the function can optionally
 *                  store the parsed IP address string of the sender. Note: The implementation in
 *                  `discovery.c` primarily relies on the IP obtained from `inet_ntop` in the calling thread.
 * @param addr_len The size of the `sender_addr` structure, needed when sending a response via `sendto`.
 * @param sender_addr A pointer to the `sockaddr_in` structure filled by `recvfrom`, containing the
 *                    sender's IP address and port number. Used to send responses directly back.
 * @return 1 If the message resulted in a *new* peer being added to the list.
 * @return 0 If the message was processed successfully but resulted in updating an *existing* peer
 *           or if it was a valid message type but didn't require adding/updating (though current logic adds/updates).
 * @return -1 If the message could not be parsed, was not a relevant discovery message type,
 *            or if adding the peer failed (e.g., list full).
 */
int handle_discovery_message(app_state_t *state, const char *buffer,
                            char *sender_ip, socklen_t addr_len,
                            struct sockaddr_in *sender_addr);

#endif // End of the include guard DISCOVERY_H