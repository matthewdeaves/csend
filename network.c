#include "peer.h"
#include <netdb.h>

int get_local_ip(char *buffer, size_t size) {
    struct ifaddrs *ifaddr, *ifa;
    int family;
    
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }
    
    // Walk through linked list, finding the first non-loopback IPv4 address
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;
            
        family = ifa->ifa_addr->sa_family;
        
        // Check it is IPv4
        if (family == AF_INET) {
            // Convert IP to string to check if it's loopback
            char ip_str[INET_ADDRSTRLEN];
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, INET_ADDRSTRLEN);
            
            // Skip loopback addresses (127.x.x.x)
            if (strncmp(ip_str, "127.", 4) == 0) {
                continue;
            }
            
            // Copy the IP address to the output buffer
            strncpy(buffer, ip_str, size);
            buffer[size-1] = '\0';
            
            freeifaddrs(ifaddr);
            return 0;
        }
    }
    
    freeifaddrs(ifaddr);
    return -1;
}

void set_socket_timeout(int socket, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

int init_listener(app_state_t *state) {
    struct sockaddr_in address;
    int opt = 1;
    
    // Create TCP socket for incoming messages
    if ((state->tcp_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("TCP socket creation failed");
        return -1;
    }
    
    // Set socket options
    if (setsockopt(state->tcp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("TCP setsockopt failed");
        close(state->tcp_socket);
        state->tcp_socket = -1;
        return -1;
    }
    
    // Configure address
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT_TCP);
    
    // Bind socket
    if (bind(state->tcp_socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("TCP bind failed");
        close(state->tcp_socket);
        state->tcp_socket = -1;
        return -1;
    }
    
    // Listen for connections
    if (listen(state->tcp_socket, 10) < 0) {
        perror("TCP listen failed");
        close(state->tcp_socket);
        state->tcp_socket = -1;
        return -1;
    }
    
    log_message("TCP listener initialized on port %d", PORT_TCP);
    return 0;
}

int send_message(const char *ip, const char *message, const char *msg_type) {
    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    app_state_t *state = g_state;
    
    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    // Set timeout
    set_socket_timeout(sock, 5);
    
    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT_TCP);
    
    // Convert IP address
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(sock);
        return -1;
    }
    
    // Connect to server
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        return -1;
    }
    
    // Format message
    format_message(buffer, BUFFER_SIZE, msg_type, state->username, message);
    
    // Send message
    if (send(sock, buffer, strlen(buffer), 0) < 0) {
        perror("Send failed");
        close(sock);
        return -1;
    }
    
    // Close socket
    close(sock);
    return 0;
}

/*
 * This function runs in a separate thread and is responsible for:
 * 1. Listening for incoming TCP connections from other peers
 * 2. Accepting connections and reading message data
 * 3. Parsing received messages and handling them based on message type
 * 4. Adding new peers to the application's peer list when messages are received
 * 5. Processing text messages and displaying them to the user
 * 6. Handling quit notifications from peers leaving the network
 * */
void *listener_thread(void *arg) {
    app_state_t *state = (app_state_t *)arg;
    struct sockaddr_in client_addr;
    int addrlen = sizeof(client_addr);
    int client_sock;
    char buffer[BUFFER_SIZE];
    char sender_ip[INET_ADDRSTRLEN];
    char sender_username[32];
    char msg_type[32];
    char content[BUFFER_SIZE];
    fd_set readfds;
    struct timeval timeout;
    
    log_message("Listener thread started");
    
    while (state->running) {
        // Set up select to make accept non-blocking
        FD_ZERO(&readfds);
        FD_SET(state->tcp_socket, &readfds);
        
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int activity = select(state->tcp_socket + 1, &readfds, NULL, NULL, &timeout);
        
        if (activity < 0 && errno != EINTR) {
            perror("Select error");
            break;
        }
        
        // Check if we should continue running
        if (!state->running) {
            break;
        }
        
        // If timeout or error, continue
        if (activity <= 0) {
            continue;
        }
        
        // Accept incoming connection
        if ((client_sock = accept(state->tcp_socket, (struct sockaddr *)&client_addr, 
                                 (socklen_t*)&addrlen)) < 0) {
            if (errno != EINTR) {
                perror("Accept failed");
            }
            continue;
        }
        
        // Get client IP
        inet_ntop(AF_INET, &client_addr.sin_addr, sender_ip, INET_ADDRSTRLEN);
        
        // Read message
        memset(buffer, 0, BUFFER_SIZE);
        if (read(client_sock, buffer, BUFFER_SIZE) > 0) {
            // Parse message
            if (parse_message(buffer, sender_ip, sender_username, msg_type, content) == 0) {
                // Add peer to list
                if (add_peer(state, sender_ip, sender_username) > 0) {
                    log_message("New peer discovered: %s@%s", sender_username, sender_ip);
                }
                
                // Handle message based on type
                if (strcmp(msg_type, MSG_TEXT) == 0) {
                    log_message("Message from %s@%s: %s", sender_username, sender_ip, content);
                }
                else if (strcmp(msg_type, MSG_QUIT) == 0) {
                    log_message("Peer %s@%s has left the network", sender_username, sender_ip);
                    
                    // Remove peer
                    pthread_mutex_lock(&state->peers_mutex);
                    for (int i = 0; i < MAX_PEERS; i++) {
                        if (state->peers[i].active && strcmp(state->peers[i].ip, sender_ip) == 0) {
                            state->peers[i].active = 0;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&state->peers_mutex);
                }
            }
        }
        
        // Close client socket
        close(client_sock);
    }
    
    log_message("Listener thread stopped");
    return NULL;
}
