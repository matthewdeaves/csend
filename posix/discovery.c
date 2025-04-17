// FILE: ./posix/discovery.c
// Include the header file for this module, which declares the functions defined here.
// It also brings in necessary types like app_state_t, socket-related headers, etc.
#include "discovery.h"

// Include utility functions, specifically for logging messages.
#include "logging.h"
// Include protocol functions for formatting and parsing discovery messages.
#include "../shared/protocol.h"
// Include network utility functions like getting the local IP and setting socket timeouts.
#include "network.h"

// Standard C library for input/output functions, primarily used here for perror to print error messages.
#include <stdio.h>
// Standard C library for string manipulation functions (memset, strlen, strcmp, strcpy).
#include <string.h>
// Standard C library for time-related functions (time, time_t).
#include <time.h>
// POSIX standard library for various functions like close (for sockets) and usleep (for pausing).
#include <unistd.h>
// Provides functions for network address manipulation (inet_ntop, htonl, htons).
#include <arpa/inet.h>
// Defines the errno variable and error constants, used implicitly by perror.
#include <errno.h>

/**
 * @brief Initializes the UDP socket used for peer discovery broadcasts and responses.
 * @details This function performs the necessary steps to set up a UDP socket
 *          that can send broadcast messages and receive responses from other peers
 *          on the local network.
 *          - Creates a UDP socket (`SOCK_DGRAM`).
 *          - Sets `SO_REUSEADDR` to allow the socket to bind to an address/port that
 *            might be in a TIME_WAIT state from a previous run, which is useful
 *            during development and restarts.
 *          - Sets `SO_BROADCAST` to enable sending messages to the broadcast address
 *            (typically 255.255.255.255 or a subnet-specific broadcast).
 *          - Binds the socket to `INADDR_ANY` (all available network interfaces on this host)
 *            and the predefined `PORT_UDP`. This allows receiving messages sent to this port.
 *          - Sets a receive timeout on the socket using `set_socket_timeout` so that
 *            the `recvfrom` call in the discovery thread doesn't block indefinitely.
 * @param state Pointer to the application's shared state structure, which holds
 *              the UDP socket file descriptor and other relevant info.
 * @return 0 on successful initialization.
 * @return -1 on failure (e.g., socket creation, option setting, or binding failed).
 *         Error details are printed to stderr via perror. The socket descriptor
 *         in the state is set to -1 on failure.
 */
int init_discovery(app_state_t *state) {
    // Structure to hold the socket address information (IP address and port).
    struct sockaddr_in address;
    // Variable used for setting socket options. A value of 1 typically means "enable".
    int opt = 1;

    // Create a UDP socket.
    // AF_INET: Address family IPv4.
    // SOCK_DGRAM: Socket type for datagrams (UDP).
    // 0: Protocol (IPPROTO_UDP, usually 0 for UDP).
    if ((state->udp_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        // perror prints a system error message corresponding to the last error (errno).
        perror("UDP socket creation failed");
        return -1; // Return failure code.
    }

    // Set socket option: SO_REUSEADDR.
    // Allows immediate reuse of the port if the application restarts quickly.
    // SOL_SOCKET: Option level (general socket options).
    // SO_REUSEADDR: The option name.
    // &opt: Pointer to the option value (1 = enable).
    // sizeof(opt): Size of the option value.
    if (setsockopt(state->udp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("UDP setsockopt(SO_REUSEADDR) failed");
        // Clean up: close the socket if options fail.
        close(state->udp_socket);
        state->udp_socket = -1; // Mark socket as invalid in the state.
        return -1;
    }

    // Set socket option: SO_BROADCAST.
    // Required to send messages to the broadcast address.
    if (setsockopt(state->udp_socket, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt))) {
        perror("UDP setsockopt(SO_BROADCAST) failed");
        // Clean up: close the socket if options fail.
        close(state->udp_socket);
        state->udp_socket = -1; // Mark socket as invalid in the state.
        return -1;
    }

    // Prepare the sockaddr_in structure for binding.
    // sin_family: Address family (IPv4).
    address.sin_family = AF_INET;
    // sin_addr.s_addr: IP address to bind to.
    // INADDR_ANY: Bind to all available local network interfaces.
    address.sin_addr.s_addr = INADDR_ANY;
    // sin_port: Port number to bind to.
    // htons: Converts the port number from host byte order to network byte order (Big Endian).
    address.sin_port = htons(PORT_UDP);

    // Bind the socket to the specified address and port.
    // This assigns the address to the socket, allowing it to receive messages sent to that address/port.
    // We cast `address` to the generic `struct sockaddr *` required by bind.
    if (bind(state->udp_socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("UDP bind failed");
        // Clean up: close the socket if bind fails.
        close(state->udp_socket);
        state->udp_socket = -1; // Mark socket as invalid in the state.
        return -1;
    }

    // Set a timeout (e.g., 1 second) for receive operations on this UDP socket.
    // This prevents the discovery thread from blocking forever in recvfrom.
    set_socket_timeout(state->udp_socket, 1);

    // Log successful initialization.
    log_message("UDP discovery initialized on port %d", PORT_UDP);
    return 0; // Return success code.
}

/**
 * @brief Broadcasts a discovery message over the network.
 * @details Constructs a discovery message using the application protocol format
 *          (including the sender's username) and sends it as a UDP broadcast packet.
 *          The message is sent to the network broadcast address (`INADDR_BROADCAST`)
 *          on the predefined `PORT_UDP`.
 * @param state Pointer to the application's shared state, used to access the
 *              UDP socket and the local username.
 * @return 0 on success.
 * @return -1 if formatting the message or sending the broadcast fails.
 *         Error details for sending are printed to stderr via perror.
 */
int broadcast_discovery(app_state_t *state) {
    // Structure to hold the destination broadcast address.
    struct sockaddr_in broadcast_addr;
    // Buffer to hold the formatted discovery message.
    char buffer[BUFFER_SIZE];
    // Buffer for local IP
    char local_ip[INET_ADDRSTRLEN];
    int formatted_len; // Store length returned by format_message

    // Get the local IP address
    if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
        log_message("Warning: broadcast_discovery failed to get local IP. Using 'unknown'.");
        strcpy(local_ip, "unknown"); // Fallback
    }

    // Format the discovery message using the defined protocol.
    // Includes message type (MSG_DISCOVERY), sender's username, and empty content.
    formatted_len = format_message(buffer, BUFFER_SIZE, MSG_DISCOVERY, state->username, local_ip, ""); // Pass local_ip
    if (formatted_len <= 0) { // format_message returns 0 on error, >0 on success
        log_message("Error: Failed to format discovery broadcast message (buffer too small?).");
        return -1;
    }

    // Prepare the broadcast address structure.
    // Initialize the structure to zero.
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    // Set address family to IPv4.
    broadcast_addr.sin_family = AF_INET;
    // Set the destination port (network byte order).
    broadcast_addr.sin_port = htons(PORT_UDP);
    // Set the destination IP address to the broadcast address.
    // INADDR_BROADCAST: Special address (usually 255.255.255.255) that sends the packet
    // to all hosts on the local network segment.
    // htonl: Converts the broadcast address constant from host byte order to network byte order.
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    // Send the formatted message using the UDP socket.
    // state->udp_socket: The socket descriptor obtained from init_discovery.
    // buffer: The message data to send.
    // formatted_len - 1: The length of the actual data (excluding null terminator).
    // 0: Flags (usually 0 for sendto).
    // (struct sockaddr *)&broadcast_addr: Pointer to the destination address structure.
    // sizeof(broadcast_addr): Size of the destination address structure.
    if (sendto(state->udp_socket, buffer, formatted_len - 1, 0, // Send actual data length
              (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr)) < 0) {
        perror("Discovery broadcast sendto failed");
        return -1; // Return failure code.
    }

    // log_message("Discovery message broadcasted."); // Optional: Log successful broadcast
    return 0; // Return success code.
}

/**
 * @brief Handles an incoming UDP message received on the discovery socket.
 * @details Parses the received buffer according to the application protocol.
 *          If it's a `MSG_DISCOVERY` from another peer:
 *          - Sends back a `MSG_DISCOVERY_RESPONSE` to the sender.
 *          - Adds or updates the sender in the local peer list.
 *          If it's a `MSG_DISCOVERY_RESPONSE` from another peer:
 *          - Adds or updates the sender in the local peer list.
 *          Messages that don't match the protocol or aren't discovery-related are ignored.
 * @param state Pointer to the application's shared state.
 * @param buffer The raw data buffer received from the socket.
 * @param bytes_read The actual number of bytes received in the buffer. <-- Added parameter
 * @param sender_ip Buffer where the parsed sender IP address will be stored (output).
 *                  This is passed in pre-allocated.
 * @param addr_len The size of the sender's address structure (`sender_addr`).
 * @param sender_addr Pointer to the `sockaddr_in` structure containing the sender's
 *                    address details (IP and port), as filled by `recvfrom`.
 * @return 1 if a new peer was added to the list.
 * @return 0 if an existing peer was updated or the message was valid but didn't result in a new peer.
 * @return -1 if the message couldn't be parsed or wasn't a relevant discovery message.
 */
int handle_discovery_message(app_state_t *state, const char *buffer, int bytes_read, // <-- Added bytes_read
                            char *sender_ip, socklen_t addr_len,
                            struct sockaddr_in *sender_addr) {
    // Buffers to store parsed message components.
    char sender_username[32]; // Max username length + null terminator
    char msg_type[32];        // Max message type length + null terminator
    char content[BUFFER_SIZE]; // Buffer for message content (though not used for discovery)
    char local_ip[INET_ADDRSTRLEN]; // Buffer for local IP
    char sender_ip_from_payload[INET_ADDRSTRLEN]; // Buffer for IP extracted by parse_message
    int response_len; // Store length of formatted response

    // Attempt to parse the received buffer using the protocol definition.
    // Pass the actual number of bytes received (bytes_read).
    // sender_ip_from_payload is used here as parse_message expects an output buffer for the IP.
    // We will primarily use the sender_ip passed into this function (from the UDP header) for adding peers.
    if (parse_message(buffer, bytes_read, sender_ip_from_payload, sender_username, msg_type, content) == 0) {
        // Check if the message type is a discovery request.
        if (strcmp(msg_type, MSG_DISCOVERY) == 0) {
            // Received a discovery request from another peer.

            // Prepare a response message buffer.
            char response[BUFFER_SIZE];

            // Get local IP before formatting response
            if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
                log_message("Warning: handle_discovery_message failed to get local IP for response. Using 'unknown'.");
                strcpy(local_ip, "unknown");
            }

            // Format the response message (type = MSG_DISCOVERY_RESPONSE, include our username and local IP).
            response_len = format_message(response, BUFFER_SIZE, MSG_DISCOVERY_RESPONSE, state->username, local_ip, "");
            if (response_len <= 0) {
                 log_message("Error: Failed to format discovery response message (buffer too small?).");
                 // Continue to add peer even if response formatting fails
            } else {
                // Send the response back directly to the sender's address and port.
                // sender_addr contains the IP/port filled by recvfrom in the calling thread.
                if (sendto(state->udp_socket, response, response_len - 1, 0, // Send actual data length
                          (struct sockaddr *)sender_addr, addr_len) < 0) {
                    perror("Discovery response sendto failed");
                    // Non-fatal error, continue processing the received discovery.
                }
            }

            // Add or update the peer who sent the discovery message in our list.
            // Use the sender_ip derived from the packet source address (passed into this function).
            int add_result = add_peer(state, sender_ip, sender_username);
            if (add_result > 0) {
                log_message("New peer discovered via DISCOVERY: %s@%s", sender_username, sender_ip);
                return 1; // Indicate a new peer was added.
            } else if (add_result == 0) {
                // Peer already existed, was just updated.
                return 0; // Indicate existing peer updated.
            } else {
                // Peer list might be full.
                log_message("Peer list full, could not add %s@%s", sender_username, sender_ip);
                return -1; // Indicate an issue (list full).
            }
        }
        // Check if the message type is a discovery response.
        else if (strcmp(msg_type, MSG_DISCOVERY_RESPONSE) == 0) {
            // Received a response to our previous discovery broadcast.

            // Add or update the peer who sent the response in our list.
            // Use the sender_ip derived from the packet source address (passed into this function).
            int add_result = add_peer(state, sender_ip, sender_username);
            if (add_result > 0) {
                log_message("New peer discovered via RESPONSE: %s@%s", sender_username, sender_ip);
                return 1; // Indicate a new peer was added.
            } else if (add_result == 0) {
                // Peer already existed, was just updated.
                return 0; // Indicate existing peer updated.
            } else {
                 // Peer list might be full.
                log_message("Peer list full, could not add %s@%s", sender_username, sender_ip);
                return -1; // Indicate an issue (list full).
            }
        }
        // If the message type is neither DISCOVERY nor DISCOVERY_RESPONSE, ignore it in this handler.
    } else {
        // Parsing failed, likely not a valid message according to our protocol (e.g., bad magic number).
        // log_message("Received UDP packet that failed to parse from %s", sender_ip); // Can be noisy
    }

    // Return -1 if parsing failed or it wasn't a relevant discovery message type.
    return -1;
}

/**
 * @brief Main function for the peer discovery thread.
 * @details This function runs in a dedicated thread and handles the ongoing
 *          process of discovering other peers and responding to their discovery attempts.
 *          It performs the following actions in a loop:
 *          1. Periodically (every `DISCOVERY_INTERVAL` seconds) broadcasts a
 *             `MSG_DISCOVERY` message using `broadcast_discovery`.
 *          2. Listens for incoming UDP packets on the discovery socket using `recvfrom`.
 *             `recvfrom` has a timeout set by `init_discovery`, so it doesn't block forever.
 *          3. If a packet is received, it extracts the sender's IP address.
 *          4. It ignores packets originating from the local IP address (itself).
 *          5. It calls `handle_discovery_message` to process valid discovery-related packets.
 *          6. Includes a short sleep (`usleep`) to prevent the loop from consuming 100% CPU
 *             if no messages are received.
 *          The loop continues as long as the `state->running` flag is true.
 * @param arg A void pointer argument, expected to be a pointer to the `app_state_t` structure.
 * @return NULL. Standard practice for POSIX thread functions.
 */
void *discovery_thread(void *arg) {
    // Cast the void pointer argument back to the expected app_state_t pointer.
    app_state_t *state = (app_state_t *)arg;
    // Structure to store the sender's address information when receiving packets.
    struct sockaddr_in sender_addr;
    // Variable to store the size of the sender_addr structure. Required by recvfrom.
    socklen_t addr_len = sizeof(sender_addr);
    // Buffer to hold incoming UDP packet data.
    char buffer[BUFFER_SIZE];
    // Buffer to store the string representation of the sender's IP address.
    char sender_ip[INET_ADDRSTRLEN]; // INET_ADDRSTRLEN is max length for IPv4 string + null term.
    // Buffer to store the local machine's primary IP address.
    char local_ip[INET_ADDRSTRLEN];
    // Variable to keep track of the time of the last broadcast.
    time_t last_broadcast = 0;
    // Variable to store the number of bytes read by recvfrom
    int bytes_read;

    // Attempt to get the primary non-loopback local IP address.
    if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
        log_message("Warning: Failed to get local IP address for discovery self-check. Using 127.0.0.1.");
        // Fallback to loopback if getting the real IP fails. This might cause issues
        // if the machine actually receives its own broadcast on 127.0.0.1, but it's better than crashing.
        strcpy(local_ip, "127.0.0.1");
    }

    log_message("Discovery thread started (local IP: %s)", local_ip);

    // Perform an initial discovery broadcast immediately when the thread starts.
    broadcast_discovery(state);
    // Record the time of this initial broadcast.
    last_broadcast = time(NULL); // Get current time in seconds since epoch.

    // Main loop: continues as long as the application is running.
    while (state->running) {
        // Check if it's time to send the next periodic broadcast.
        if (time(NULL) - last_broadcast >= DISCOVERY_INTERVAL) {
            // If enough time has passed, send another broadcast.
            broadcast_discovery(state);
            // Update the time of the last broadcast.
            last_broadcast = time(NULL);
        }

        // Attempt to receive a UDP packet.
        // Clear the buffer before receiving.
        memset(buffer, 0, BUFFER_SIZE);
        // recvfrom waits for a packet or times out (timeout set in init_discovery).
        // state->udp_socket: The socket to listen on.
        // buffer: Where to store received data.
        // BUFFER_SIZE: Max bytes to read.
        // 0: Flags (MSG_DONTWAIT could be used, but timeout is set instead).
        // (struct sockaddr *)&sender_addr: Pointer to store sender's address info.
        // &addr_len: Pointer to the size of the address structure (input/output).
        bytes_read = recvfrom(state->udp_socket, buffer, BUFFER_SIZE -1, 0, // Read BUFFER_SIZE-1 to ensure null termination possible
                                 (struct sockaddr *)&sender_addr, &addr_len);

        // Ensure null termination after read, just in case (though recvfrom doesn't guarantee it for UDP)
        // It's safer to rely on bytes_read for the actual data length.
        // buffer[BUFFER_SIZE - 1] = '\0'; // Optional safety net

        // Check if data was successfully received (bytes_read > 0).
        // recvfrom returns -1 on error (or if timeout occurs and errno is EAGAIN/EWOULDBLOCK).
        if (bytes_read > 0) {
            // Convert the sender's binary IP address (from sender_addr) to a string.
            // AF_INET: Address family.
            // &sender_addr.sin_addr: Pointer to the binary IP address structure.
            // sender_ip: Buffer to store the resulting string.
            // INET_ADDRSTRLEN: Size of the buffer.
            inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, INET_ADDRSTRLEN);

            // Check if the received message is from our own IP address.
            // If so, ignore it to prevent processing our own broadcasts/responses.
            if (strcmp(sender_ip, local_ip) == 0) {
                // log_message("Ignoring discovery message from self (%s)", sender_ip); // Optional debug log
                continue; // Skip the rest of the loop and wait for the next packet/timeout.
            }

            // If the message is not from self, process it.
            // Pass the received data, the actual number of bytes read, sender's IP string, address length, and address structure.
            handle_discovery_message(state, buffer, bytes_read, sender_ip, addr_len, &sender_addr); // Pass bytes_read

        } else if (bytes_read < 0) {
            // An error occurred or the timeout was reached.
            // Check errno. EAGAIN or EWOULDBLOCK simply mean the timeout occurred, which is normal.
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // An actual error occurred during recvfrom.
                perror("Discovery recvfrom error");
                // Depending on the error, might want to break or handle differently.
                // For now, just log and continue.
            }
            // If it was just a timeout (EAGAIN/EWOULDBLOCK), do nothing and loop again.
        }
        // If bytes_read == 0, it typically means the peer disconnected gracefully (more relevant for TCP).
        // For UDP, it's less common but could happen. We just loop again.


        // Introduce a small delay (100 milliseconds) in each loop iteration.
        // This prevents the `while` loop from constantly checking the time and
        // calling `recvfrom` immediately after a timeout, which would waste CPU cycles.
        // It yields control, allowing other threads/processes to run.
        usleep(100000); // 100,000 microseconds = 100 milliseconds.
    }

    // Log when the thread is stopping (because state->running became false).
    log_message("Discovery thread stopped");
    return NULL; // Standard return for thread functions.
}