#include "discovery.h"
#include "logging.h"
#include "../shared/protocol.h"
#include "../shared/discovery.h"
#include "../shared/logging.h"
#include "network.h"
#include "peer.h"
#include "ui_interface.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
static void posix_send_discovery_response(uint32_t dest_ip_addr_host, uint16_t dest_port_host, void *platform_context)
{
    app_state_t *state = (app_state_t *)platform_context;
    char response[BUFFER_SIZE];
    char local_ip[INET_ADDRSTRLEN];
    int response_len;
    struct sockaddr_in dest_addr;
    if (!state || state->udp_socket < 0) {
        log_debug("Error (posix_send_discovery_response): Invalid state or UDP socket.");
        return;
    }
    if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
        log_debug("Warning: posix_send_discovery_response failed to get local IP. Using 'unknown'.");
        strcpy(local_ip, "unknown");
    }
    response_len = format_message(response, BUFFER_SIZE, MSG_DISCOVERY_RESPONSE, state->username, local_ip, "");
    if (response_len <= 0) {
        log_debug("Error: Failed to format discovery response message (buffer too small?).");
        return;
    }
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = htonl(dest_ip_addr_host);
    dest_addr.sin_port = htons(dest_port_host);
    if (sendto(state->udp_socket, response, response_len - 1, 0,
               (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        perror("Discovery response sendto failed");
    } else {
        log_debug("Sent DISCOVERY_RESPONSE to %s:%u", inet_ntoa(dest_addr.sin_addr), dest_port_host);
    }
}
static int posix_add_or_update_peer(const char *ip, const char *username, void *platform_context)
{
    app_state_t *state = (app_state_t *)platform_context;
    if (!state) return -1;
    return add_peer(state, ip, username);
}
static void posix_notify_peer_list_updated(void *platform_context)
{
    app_state_t *state = (app_state_t *)platform_context;
    
    if (state && state->ui) {
        UI_CALL(state->ui, notify_peer_update);
    } else {
        log_debug("posix_notify_peer_list_updated called (no UI available).");
    }
}
int init_discovery(app_state_t *state)
{
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
    log_debug("UDP discovery initialized on port %d", PORT_UDP);
    return 0;
}
int broadcast_discovery(app_state_t *state)
{
    struct sockaddr_in broadcast_addr;
    char buffer[BUFFER_SIZE];
    char local_ip[INET_ADDRSTRLEN];
    int formatted_len;
    if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
        log_debug("Warning: broadcast_discovery failed to get local IP. Using 'unknown'.");
        strcpy(local_ip, "unknown");
    }
    formatted_len = format_message(buffer, BUFFER_SIZE, MSG_DISCOVERY, state->username, local_ip, "");
    if (formatted_len <= 0) {
        log_debug("Error: Failed to format discovery broadcast message (buffer too small?).");
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
    log_debug("Discovery broadcast sent.");
    return 0;
}
void *discovery_thread(void *arg)
{
    app_state_t *state = (app_state_t *)arg;
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);
    char buffer[BUFFER_SIZE];
    char sender_ip_str[INET_ADDRSTRLEN];
    char local_ip_str[INET_ADDRSTRLEN];
    time_t last_broadcast = 0;
    int bytes_read;
    discovery_platform_callbacks_t callbacks = {
        .send_response_callback = posix_send_discovery_response,
        .add_or_update_peer_callback = posix_add_or_update_peer,
        .notify_peer_list_updated_callback = posix_notify_peer_list_updated
    };
    if (get_local_ip(local_ip_str, INET_ADDRSTRLEN) < 0) {
        log_debug("Warning: Failed to get local IP address for discovery self-check. Using 127.0.0.1.");
        strcpy(local_ip_str, "127.0.0.1");
    }
    log_debug("Discovery thread started (local IP: %s)", local_ip_str);
    broadcast_discovery(state);
    last_broadcast = time(NULL);
    while (state->running) {
        if (time(NULL) - last_broadcast >= DISCOVERY_INTERVAL) {
            broadcast_discovery(state);
            last_broadcast = time(NULL);
        }
        memset(buffer, 0, BUFFER_SIZE);
        bytes_read = recvfrom(state->udp_socket, buffer, BUFFER_SIZE - 1, 0,
                              (struct sockaddr *)&sender_addr, &addr_len);
        if (bytes_read > 0) {
            inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip_str, INET_ADDRSTRLEN);
            if (strcmp(sender_ip_str, local_ip_str) == 0) {
                log_debug("Ignored discovery packet from self (%s).", sender_ip_str);
                continue;
            }
            uint32_t sender_ip_addr_host = ntohl(sender_addr.sin_addr.s_addr);
            uint16_t sender_port_host = ntohs(sender_addr.sin_port);
            discovery_logic_process_packet(buffer, bytes_read,
                                           sender_ip_str, sender_ip_addr_host, sender_port_host,
                                           &callbacks,
                                           state);
        } else if (bytes_read < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                if (state->running) {
                    perror("Discovery recvfrom error");
                }
            }
        }
        usleep(100000);
    }
    log_debug("Discovery thread stopped");
    return NULL;
}
