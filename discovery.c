#include "discovery.h"
#include <unistd.h>

/**
 * Initializes the UDP socket for peer discovery
 *
 * This function sets up a UDP socket for peer discovery by:
 * - Creating a new UDP socket
 * - Setting socket options (SO_REUSEADDR)
 * - Enabling broadcast capability (SO_BROADCAST)
 * - Binding the socket to the configured UDP port
 * - Setting a socket timeout for non-blocking operations
 *
 * The socket is configured to listen on all available network interfaces (INADDR_ANY)
 * and will be used for both sending and receiving discovery messages.
 *
 * int Return status:
 *         0: Success - UDP socket initialized and ready
 *        -1: Failure - Socket creation, option setting, or binding failed
 *            (error details are logged via perror)
 *
 * On failure, the function ensures the socket is closed and set to -1 in the state
 */
int init_discovery(app_state_t *state) {
    struct sockaddr_in address;
    int opt = 1;
    
    // Create UDP socket for discovery
    if ((state->udp_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("UDP socket creation failed");
        return -1;
    }
    
    // Set socket options
    if (setsockopt(state->udp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("UDP setsockopt failed");
        close(state->udp_socket);
        state->udp_socket = -1;
        return -1;
    }
    
    // Enable broadcast
    if (setsockopt(state->udp_socket, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt))) {
        perror("UDP broadcast setsockopt failed");
        close(state->udp_socket);
        state->udp_socket = -1;
        return -1;
    }
    
    // Configure address
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT_UDP);
    
    // Bind socket
    if (bind(state->udp_socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("UDP bind failed");
        close(state->udp_socket);
        state->udp_socket = -1;
        return -1;
    }
    
    // Set timeout for UDP socket
    set_socket_timeout(state->udp_socket, 1);
    
    log_message("UDP discovery initialized on port %d", PORT_UDP);
    return 0;
}

int broadcast_discovery(app_state_t *state) {
    struct sockaddr_in broadcast_addr;
    char buffer[BUFFER_SIZE];
    
    // Format discovery message
    format_message(buffer, BUFFER_SIZE, MSG_DISCOVERY, state->username, "");
    
    // Set up broadcast address
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(PORT_UDP);
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    
    // Send broadcast
    if (sendto(state->udp_socket, buffer, strlen(buffer), 0,
              (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr)) < 0) {
        perror("Discovery broadcast failed");
        return -1;
    }
    
    return 0;
}

/*
 * Processes incoming discovery messages and responds accordingly
 *
 * This function handles two types of discovery messages:
 * - MSG_DISCOVERY: Initial discovery requests from other peers
 * - MSG_DISCOVERY_RESPONSE: Responses to our discovery requests
 *
 * When a discovery message is received, the function responds with a 
 * discovery response and adds the sender to the peer list. When a
 * discovery response is received, the sender is added to the peer list.
 *
 * Return status:
 *         1: New peer was discovered and added
 *         0: Existing peer was found (no addition)
 *        -1: Not a discovery message or error occurred
*/
int handle_discovery_message(app_state_t *state, const char *buffer, 
                            char *sender_ip, socklen_t addr_len,
                            struct sockaddr_in *sender_addr) {
    char sender_username[32];
    char msg_type[32];
    char content[BUFFER_SIZE];
    
    // Parse message
    if (parse_message(buffer, sender_ip, sender_username, msg_type, content) == 0) {
        if (strcmp(msg_type, MSG_DISCOVERY) == 0) {
            // Respond to discovery
            char response[BUFFER_SIZE];
            format_message(response, BUFFER_SIZE, MSG_DISCOVERY_RESPONSE, state->username, "");
            
            sendto(state->udp_socket, response, strlen(response), 0,
                  (struct sockaddr *)sender_addr, addr_len);
            
            // Add peer to list
            if (add_peer(state, sender_ip, sender_username) > 0) {
                log_message("New peer discovered: %s@%s", sender_username, sender_ip);
                return 1; // New peer
            }
            return 0; // Existing peer
        }
        else if (strcmp(msg_type, MSG_DISCOVERY_RESPONSE) == 0) {
            // Add peer to list
            if (add_peer(state, sender_ip, sender_username) > 0) {
                log_message("New peer discovered: %s@%s", sender_username, sender_ip);
                return 1; // New peer
            }
            return 0; // Existing peer
        }
    }
    return -1; // Not a discovery message
}

/*
 * This function runs in a separate thread and is responsible for:
 * 1. Periodically broadcasting discovery messages to find other peers
 * 2. Listening for discovery messages from other peers
 * 3. Responding to discovery messages with discovery response messages
 * 4. Adding discovered peers to the application's peer list
 * */
void *discovery_thread(void *arg) {
    app_state_t *state = (app_state_t *)arg;
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);
    char buffer[BUFFER_SIZE];
    char sender_ip[INET_ADDRSTRLEN];
    char local_ip[INET_ADDRSTRLEN];
    time_t last_broadcast = 0;
    
    // Get local IP address
    if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
        log_message("Failed to get local IP address");
        strcpy(local_ip, "127.0.0.1");
    }
    
    log_message("Discovery thread started (local IP: %s)", local_ip);
    
    // Initial discovery broadcast
    broadcast_discovery(state);
    last_broadcast = time(NULL);
    
    while (state->running) {
        // Periodically broadcast discovery message
        if (time(NULL) - last_broadcast >= DISCOVERY_INTERVAL) {
            broadcast_discovery(state);
            last_broadcast = time(NULL);
        }
        
        // Check for incoming discovery messages
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = recvfrom(state->udp_socket, buffer, BUFFER_SIZE, 0,
                                 (struct sockaddr *)&sender_addr, &addr_len);
        
        if (bytes_read > 0) {
            // Get sender IP
            inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, INET_ADDRSTRLEN);
            
            // Skip messages from self
            if (strcmp(sender_ip, local_ip) == 0) {
                continue;
            }
            
            // Handle the discovery message
            handle_discovery_message(state, buffer, sender_ip, addr_len, &sender_addr);
        }
        
        // Small delay to prevent CPU hogging
        usleep(100000); // 100ms
    }
    
    log_message("Discovery thread stopped");
    return NULL;
}