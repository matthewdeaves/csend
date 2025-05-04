#include "messaging.h"
#include "network.h"
#include "../shared/protocol.h"
#include "logging.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
int init_listener(app_state_t *state) {
    struct sockaddr_in address;
    int opt = 1;
    if ((state->tcp_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("TCP socket creation failed");
        return -1;
    }
    if (setsockopt(state->tcp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("TCP setsockopt(SO_REUSEADDR) failed");
        close(state->tcp_socket);
        state->tcp_socket = -1;
        return -1;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT_TCP);
    if (bind(state->tcp_socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("TCP bind failed");
        close(state->tcp_socket);
        state->tcp_socket = -1;
        return -1;
    }
    if (listen(state->tcp_socket, 10) < 0) {
        perror("TCP listen failed");
        close(state->tcp_socket);
        state->tcp_socket = -1;
        return -1;
    }
    log_message("TCP listener initialized on port %d", PORT_TCP);
    return 0;
}
int send_message(const char *ip, const char *message, const char *msg_type, const char *sender_username) {
    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    char local_ip[INET_ADDRSTRLEN];
    int formatted_len;
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Outgoing TCP socket creation failed");
        return -1;
    }
    set_socket_timeout(sock, 5);
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT_TCP);
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid target IP address format");
        close(sock);
        return -1;
    }
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return -1;
    }
    if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
        log_message("Warning: send_message failed to get local IP. Using 'unknown'.");
        strcpy(local_ip, "unknown");
    }
    formatted_len = format_message(buffer, BUFFER_SIZE, msg_type, sender_username, local_ip, message);
    if (formatted_len <= 0) {
        log_message("Error: Failed to format outgoing message (buffer too small?).");
        close(sock);
        return -1;
    }
    if (send(sock, buffer, formatted_len - 1, 0) < 0) {
        perror("TCP send failed");
        close(sock);
        return -1;
    }
    close(sock);
    return 0;
}
void *listener_thread(void *arg) {
    app_state_t *state = (app_state_t *)arg;
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);
    int client_sock;
    char buffer[BUFFER_SIZE];
    char sender_ip[INET_ADDRSTRLEN];
    char sender_ip_from_payload[INET_ADDRSTRLEN];
    char sender_username[32];
    char msg_type[32];
    char content[BUFFER_SIZE];
    fd_set readfds;
    struct timeval timeout;
    ssize_t bytes_read;
    log_message("Listener thread started");
    while (state->running) {
        FD_ZERO(&readfds);
        FD_SET(state->tcp_socket, &readfds);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        int activity = select(state->tcp_socket + 1, &readfds, NULL, NULL, &timeout);
        if (activity < 0 && errno != EINTR) {
            perror("Select error in listener thread");
            break;
        }
        if (!state->running) {
            break;
        }
        if (activity == 0) {
            continue;
        }
        if ((client_sock = accept(state->tcp_socket, (struct sockaddr *)&client_addr, &addrlen)) < 0) {
            if (errno != EINTR && state->running) {
                perror("TCP accept failed");
            }
            continue;
        }
        inet_ntop(AF_INET, &client_addr.sin_addr, sender_ip, INET_ADDRSTRLEN);
        memset(buffer, 0, BUFFER_SIZE);
        bytes_read = read(client_sock, buffer, BUFFER_SIZE - 1);
        if (bytes_read > 0) {
            if (parse_message(buffer, bytes_read, sender_ip_from_payload, sender_username, msg_type, content) == 0) {
                int add_result = add_peer(state, sender_ip, sender_username);
                if (add_result > 0) {
                    log_message("New peer connected via TCP: %s@%s", sender_username, sender_ip);
                } else if (add_result < 0) {
                    log_message("Peer list full, could not add %s@%s from TCP connection", sender_username, sender_ip);
                }
                if (strcmp(msg_type, MSG_TEXT) == 0) {
                    log_message("Message from %s@%s: %s", sender_username, sender_ip, content);
                } else if (strcmp(msg_type, MSG_QUIT) == 0) {
                    log_message("Peer %s@%s has sent QUIT notification", sender_username, sender_ip);
                    pthread_mutex_lock(&state->peers_mutex);
                    for (int i = 0; i < MAX_PEERS; i++) {
                        if (state->peer_manager.peers[i].active && strcmp(state->peer_manager.peers[i].ip, sender_ip) == 0) {
                            state->peer_manager.peers[i].active = 0;
                            log_message("Marked peer %s@%s as inactive.", state->peer_manager.peers[i].username, state->peer_manager.peers[i].ip);
                            break;
                        }
                    }
                    pthread_mutex_unlock(&state->peers_mutex);
                }
            } else {
                log_message("Failed to parse TCP message from %s (%ld bytes)", sender_ip, bytes_read);
            }
        } else if (bytes_read == 0) {
            log_message("Peer %s disconnected.", sender_ip);
        } else {
            if (state->running) {
               perror("TCP read failed");
            }
        }
        close(client_sock);
    }
    log_message("Listener thread stopped");
    return NULL;
}
