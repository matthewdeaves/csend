#include "messaging.h"
#include "network.h"
#include "../shared/protocol.h"
#include "../shared/messaging.h"
#include "../shared/logging.h"
#include "../shared/peer_wrapper.h"
#include "logging.h"
#include "ui_terminal.h"
#include "ui_interface.h"
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
static int posix_tcp_add_or_update_peer(const char *ip, const char *username, void *platform_context)
{
    (void)platform_context;
    return pw_add_or_update(ip, username);
}

static void posix_tcp_display_text_message(const char *username, const char *ip, const char *message_content, void *platform_context)
{
    app_state_t *state = (app_state_t *)platform_context;
    log_app_event("%s@%s: %s", username, ip, message_content);

    if (state && state->ui) {
        UI_CALL(state->ui, display_message, username, ip, message_content);
    } else {
        /* Fallback if no UI available */
        terminal_display_app_message("%s@%s: %s", username, ip, message_content);
    }
}

static void posix_tcp_mark_peer_inactive(const char *ip, void *platform_context)
{
    (void)platform_context;
    pw_mark_inactive(ip);
}
int init_listener(app_state_t *state)
{
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
    log_info_cat(LOG_CAT_NETWORKING, "TCP listener initialized on port %d", PORT_TCP);
    return 0;
}
int send_message(const char *ip, const char *message, const char *msg_type, const char *sender_username)
{
    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    char local_ip[INET_ADDRSTRLEN];
    int formatted_len;
    int connect_attempts = 0;
    const int max_retries = 5;
    int retry_delay_ms = 50; /* Start with 50ms, exponential backoff */

    /* Retry loop for connection attempts
     * MacTCP limitation: Single listen stream can only accept one connection at a time.
     * When messages arrive simultaneously, subsequent connections get ECONNREFUSED.
     * Solution: Retry with exponential backoff (50ms, 100ms, 200ms, 400ms, 800ms) to
     * allow MacTCP to restart listening between connection attempts.
     *
     * Per testing: 10ms/20ms/40ms delays too short. MacTCP needs ~50-200ms to:
     * 1. Accept connection (ASR fires)
     * 2. Process message (parse, update peers, display UI)
     * 3. Close connection (TCPAbort)
     * 4. Restart listening (TCPPassiveOpen)
     * 5. Be ready for next connection */
    for (connect_attempts = 0; connect_attempts <= max_retries; connect_attempts++) {
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

        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) {
            /* Connection successful - break out of retry loop */
            break;
        }

        /* Connection failed - check if we should retry */
        int saved_errno = errno;
        close(sock);

        if (saved_errno == ECONNREFUSED && connect_attempts < max_retries) {
            /* Connection refused - peer's listen stream may be busy processing another message.
             * Wait briefly and retry. Log only on first retry to avoid log spam. */
            if (connect_attempts == 0) {
                log_debug_cat(LOG_CAT_NETWORKING, "Connection to %s refused (listen busy), retrying with backoff...", ip);
            }
            usleep(retry_delay_ms * 1000); /* Convert ms to microseconds */
            retry_delay_ms *= 2; /* Exponential backoff: 10ms -> 20ms -> 40ms */
            continue; /* Retry */
        }

        /* Non-retryable error or max retries exceeded */
        if (saved_errno == ECONNREFUSED || saved_errno == EHOSTUNREACH || saved_errno == ENETUNREACH) {
            log_warning_cat(LOG_CAT_NETWORKING, "Cannot reach peer %s:%d - %s (peer may be offline)", ip, PORT_TCP, strerror(saved_errno));
        } else {
            log_error_cat(LOG_CAT_NETWORKING, "Failed to connect to %s:%d - %s", ip, PORT_TCP, strerror(saved_errno));
        }
        return -1;
    }

    /* If we get here, either connect succeeded or we exhausted retries */
    if (connect_attempts > max_retries) {
        log_warning_cat(LOG_CAT_NETWORKING, "Connection to %s:%d failed after %d attempts", ip, PORT_TCP, max_retries + 1);
        return -1;
    }

    if (connect_attempts > 0) {
        log_debug_cat(LOG_CAT_NETWORKING, "Connection to %s succeeded after %d retries", ip, connect_attempts);
    }
    if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
        log_warning_cat(LOG_CAT_NETWORKING, "Warning: send_message failed to get local IP. Using 'unknown'.");
        strcpy(local_ip, "unknown");
    }
    formatted_len = format_message(buffer, BUFFER_SIZE, msg_type, generate_message_id(),
                                   sender_username, local_ip, message);
    if (formatted_len <= 0) {
        log_error_cat(LOG_CAT_MESSAGING, "Error: Failed to format outgoing message (buffer too small?).");
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
void *listener_thread(void *arg)
{
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
    tcp_platform_callbacks_t posix_callbacks = {
        .add_or_update_peer = posix_tcp_add_or_update_peer,
        .display_text_message = posix_tcp_display_text_message,
        .mark_peer_inactive = posix_tcp_mark_peer_inactive
    };
    log_info_cat(LOG_CAT_NETWORKING, "Listener thread started");
    while (state->running) {
        FD_ZERO(&readfds);
        FD_SET(state->tcp_socket, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000;  /* 50ms for more responsive shutdown */
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
        log_debug_cat(LOG_CAT_NETWORKING, "Accepted connection from %s", sender_ip);
        memset(buffer, 0, BUFFER_SIZE);
        bytes_read = read(client_sock, buffer, BUFFER_SIZE - 1);
        if (bytes_read > 0) {
            csend_uint32_t msg_id;
            if (parse_message(buffer, bytes_read, sender_ip_from_payload, sender_username, msg_type, &msg_id, content) == 0) {
                log_debug_cat(LOG_CAT_MESSAGING, "Received message ID %lu from %s@%s",
                              (unsigned long)msg_id, sender_username, sender_ip);
                handle_received_tcp_message(sender_ip, sender_username, msg_type, content,
                                            &posix_callbacks, state);
            } else {
                log_error_cat(LOG_CAT_MESSAGING, "Failed to parse TCP message from %s (%ld bytes)", sender_ip, bytes_read);
            }
        } else if (bytes_read == 0) {
            log_info_cat(LOG_CAT_NETWORKING, "Peer %s disconnected.", sender_ip);
        } else {
            if (state->running) {
                perror("TCP read failed");
            }
        }
        close(client_sock);
    }
    log_info_cat(LOG_CAT_NETWORKING, "Listener thread stopped");
    return NULL;
}
