#include "peer.h"
#include "signal_handler.h"
#include "../shared/logging.h"
#include "../shared/peer.h"
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <signal.h>
#include <pthread.h>
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
    log_info_cat(LOG_CAT_SYSTEM, "Starting POSIX cleanup...");
    if (state->tcp_socket >= 0) {
        log_debug_cat(LOG_CAT_NETWORKING, "Closing TCP socket %d", state->tcp_socket);
        close(state->tcp_socket);
        state->tcp_socket = -1;
    }
    if (state->udp_socket >= 0) {
        log_debug_cat(LOG_CAT_NETWORKING, "Closing UDP socket %d", state->udp_socket);
        close(state->udp_socket);
        state->udp_socket = -1;
    }
    log_debug_cat(LOG_CAT_SYSTEM, "Destroying peers mutex");
    pthread_mutex_destroy(&state->peers_mutex);
    log_info_cat(LOG_CAT_SYSTEM, "POSIX cleanup complete");
    g_state = NULL;
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
