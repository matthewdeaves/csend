#include "discovery.h"
#include "logging.h"
#include "../shared/protocol.h"
#include "network.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
int init_discovery(app_state_t *state) {
    struct sockaddr_in address;
    int opt = 1;
    if ((state->udp_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("UDP socket creation failed");
        return -1;
    }
    if (setsockopt(state->udp_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("UDP setsockopt(SO_REUSEADDR) failed");
        close(state->udp_socket);
        state->udp_socket = -1;
        return -1;
    }
    if (setsockopt(state->udp_socket, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt))) {
        perror("UDP setsockopt(SO_BROADCAST) failed");
        close(state->udp_socket);
        state->udp_socket = -1;
        return -1;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT_UDP);
    if (bind(state->udp_socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("UDP bind failed");
        close(state->udp_socket);
        state->udp_socket = -1;
        return -1;
    }
    set_socket_timeout(state->udp_socket, 1);
    log_message("UDP discovery initialized on port %d", PORT_UDP);
    return 0;
}
int broadcast_discovery(app_state_t *state) {
    struct sockaddr_in broadcast_addr;
    char buffer[BUFFER_SIZE];
    char local_ip[INET_ADDRSTRLEN];
    int formatted_len;
    if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
        log_message("Warning: broadcast_discovery failed to get local IP. Using 'unknown'.");
        strcpy(local_ip, "unknown");
    }
    formatted_len = format_message(buffer, BUFFER_SIZE, MSG_DISCOVERY, state->username, local_ip, "");
    if (formatted_len <= 0) {
        log_message("Error: Failed to format discovery broadcast message (buffer too small?).");
        return -1;
    }
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(PORT_UDP);
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    if (sendto(state->udp_socket, buffer, formatted_len - 1, 0,
              (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr)) < 0) {
        perror("Discovery broadcast sendto failed");
        return -1;
    }
    return 0;
}
int handle_discovery_message(app_state_t *state, const char *buffer, int bytes_read,
                            char *sender_ip, socklen_t addr_len,
                            struct sockaddr_in *sender_addr) {
    char sender_username[32];
    char msg_type[32];
    char content[BUFFER_SIZE];
    char local_ip[INET_ADDRSTRLEN];
    char sender_ip_from_payload[INET_ADDRSTRLEN];
    int response_len;
    if (parse_message(buffer, bytes_read, sender_ip_from_payload, sender_username, msg_type, content) == 0) {
        if (strcmp(msg_type, MSG_DISCOVERY) == 0) {
            char response[BUFFER_SIZE];
            if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
                log_message("Warning: handle_discovery_message failed to get local IP for response. Using 'unknown'.");
                strcpy(local_ip, "unknown");
            }
            response_len = format_message(response, BUFFER_SIZE, MSG_DISCOVERY_RESPONSE, state->username, local_ip, "");
            if (response_len <= 0) {
                 log_message("Error: Failed to format discovery response message (buffer too small?).");
            } else {
                if (sendto(state->udp_socket, response, response_len - 1, 0,
                          (struct sockaddr *)sender_addr, addr_len) < 0) {
                    perror("Discovery response sendto failed");
                }
            }
            int add_result = add_peer(state, sender_ip, sender_username);
            if (add_result > 0) {
                log_message("New peer discovered via DISCOVERY: %s@%s", sender_username, sender_ip);
                return 1;
            } else if (add_result == 0) {
                return 0;
            } else {
                log_message("Peer list full, could not add %s@%s", sender_username, sender_ip);
                return -1;
            }
        }
        else if (strcmp(msg_type, MSG_DISCOVERY_RESPONSE) == 0) {
            int add_result = add_peer(state, sender_ip, sender_username);
            if (add_result > 0) {
                log_message("New peer discovered via RESPONSE: %s@%s", sender_username, sender_ip);
                return 1;
            } else if (add_result == 0) {
                return 0;
            } else {
                log_message("Peer list full, could not add %s@%s", sender_username, sender_ip);
                return -1;
            }
        }
    } else {
    }
    return -1;
}
void *discovery_thread(void *arg) {
    app_state_t *state = (app_state_t *)arg;
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);
    char buffer[BUFFER_SIZE];
    char sender_ip[INET_ADDRSTRLEN];
    char local_ip[INET_ADDRSTRLEN];
    time_t last_broadcast = 0;
    int bytes_read;
    if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
        log_message("Warning: Failed to get local IP address for discovery self-check. Using 127.0.0.1.");
        strcpy(local_ip, "127.0.0.1");
    }
    log_message("Discovery thread started (local IP: %s)", local_ip);
    broadcast_discovery(state);
    last_broadcast = time(NULL);
    while (state->running) {
        if (time(NULL) - last_broadcast >= DISCOVERY_INTERVAL) {
            broadcast_discovery(state);
            last_broadcast = time(NULL);
        }
        memset(buffer, 0, BUFFER_SIZE);
        bytes_read = recvfrom(state->udp_socket, buffer, BUFFER_SIZE -1, 0,
                                 (struct sockaddr *)&sender_addr, &addr_len);
        if (bytes_read > 0) {
            inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, INET_ADDRSTRLEN);
            if (strcmp(sender_ip, local_ip) == 0) {
                continue;
            }
            handle_discovery_message(state, buffer, bytes_read, sender_ip, addr_len, &sender_addr);
        } else if (bytes_read < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("Discovery recvfrom error");
            }
        }
        usleep(100000);
    }
    log_message("Discovery thread stopped");
    return NULL;
}
