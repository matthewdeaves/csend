#include "peer.h"
#include "discovery.h"
#include "messaging.h"
#include "ui_terminal.h"
#include "signal_handler.h"
#include "logging.h"
#include "logging.h"
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
app_state_t *g_state = NULL;
void init_app_state(app_state_t *state, const char *username)
{
    memset(state, 0, sizeof(app_state_t));
    state->running = 1;
    state->tcp_socket = -1;
    state->udp_socket = -1;
    peer_shared_init_list(&state->peer_manager);
    strncpy(state->username, username, sizeof(state->username) - 1);
    state->username[sizeof(state->username) - 1] = '\0';
    pthread_mutex_init(&state->peers_mutex, NULL);
    g_state = state;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}
void cleanup_app_state(app_state_t *state)
{
    log_internal_message("Starting POSIX cleanup...");
    if (state->tcp_socket >= 0) {
        log_internal_message("Closing TCP socket %d", state->tcp_socket);
        close(state->tcp_socket);
        state->tcp_socket = -1;
    }
    if (state->udp_socket >= 0) {
        log_internal_message("Closing UDP socket %d", state->udp_socket);
        close(state->udp_socket);
        state->udp_socket = -1;
    }
    log_internal_message("Destroying peers mutex");
    pthread_mutex_destroy(&state->peers_mutex);
    log_internal_message("POSIX cleanup complete");
}
int add_peer(app_state_t *state, const char *ip, const char *username)
{
    if (!state) return -1;
    int result;
    pthread_mutex_lock(&state->peers_mutex);
    result = peer_shared_add_or_update(&state->peer_manager, ip, username);
    pthread_mutex_unlock(&state->peers_mutex);
    return result;
}
int prune_peers(app_state_t *state)
{
    if (!state) return 0;
    int count;
    pthread_mutex_lock(&state->peers_mutex);
    count = peer_shared_prune_timed_out(&state->peer_manager);
    pthread_mutex_unlock(&state->peers_mutex);
    return count;
}
int main(int argc, char *argv[])
{
    app_state_t state;
    pthread_t listener_tid, discovery_tid, input_tid;
    char username[32] = "anonymous";
    if (argc > 1) {
        strncpy(username, argv[1], sizeof(username) - 1);
        username[sizeof(username) - 1] = '\0';
    }
    init_app_state(&state, username);
    log_init("csend_posix.log", posix_platform_display_debug_log);
    log_app_event("Starting P2P messaging application as '%s'", state.username);
    terminal_display_app_message("Starting P2P messaging application as '%s'", state.username);
    if (init_listener(&state) < 0) {
        log_internal_message("Fatal: Failed to initialize TCP listener. Exiting.");
        log_shutdown();
        return EXIT_FAILURE;
    }
    if (init_discovery(&state) < 0) {
        log_internal_message("Fatal: Failed to initialize UDP discovery. Exiting.");
        if (state.tcp_socket >= 0) close(state.tcp_socket);
        log_shutdown();
        return EXIT_FAILURE;
    }
    int listener_err = pthread_create(&listener_tid, NULL, listener_thread, &state);
    int discovery_err = pthread_create(&discovery_tid, NULL, discovery_thread, &state);
    int input_err = pthread_create(&input_tid, NULL, user_input_thread, &state);
    if (listener_err != 0 || discovery_err != 0 || input_err != 0) {
        log_internal_message("Fatal: Failed to create one or more threads. Exiting.");
        state.running = 0;
        usleep(100000);
        if (state.tcp_socket >= 0) close(state.tcp_socket);
        if (state.udp_socket >= 0) close(state.udp_socket);
        pthread_mutex_destroy(&state.peers_mutex);
        log_shutdown();
        return EXIT_FAILURE;
    }
    log_internal_message("Threads created successfully.");
    pthread_join(input_tid, NULL);
    log_internal_message("User input thread finished. Initiating shutdown...");
    state.running = 0;
    log_internal_message("Waiting for listener thread to finish...");
    pthread_join(listener_tid, NULL);
    log_internal_message("Listener thread joined.");
    log_internal_message("Waiting for discovery thread to finish...");
    pthread_join(discovery_tid, NULL);
    log_internal_message("Discovery thread joined.");
    cleanup_app_state(&state);
    log_app_event("Application terminated gracefully.");
    terminal_display_app_message("Application terminated gracefully.");
    log_shutdown();
    return EXIT_SUCCESS;
}
