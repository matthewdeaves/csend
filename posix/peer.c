// FILE: ./posix/peer.c
#include "peer.h"
#include "peer_shared.h" // Include the shared peer logic header
// Include headers for other modules providing necessary functionality.
#include "discovery.h"
#include "messaging.h"
#include "ui_terminal.h"
#include "signal_handler.h"
#include "logging.h"

// --- Standard Library Includes ---
#include <string.h>     // For memory manipulation (memset) and string operations (strncpy, strcmp)
#include <unistd.h>     // For POSIX standard functions like close() (sockets) and usleep() (delay)
#include <stdlib.h>     // For general utilities like EXIT_SUCCESS, EXIT_FAILURE
#include <stdio.h>      // For standard I/O functions (used indirectly, e.g., by logging or error reporting)
#include <signal.h>     // For signal handling functions (sigaction, struct sigaction) and constants (SIGINT, SIGTERM)
#include <pthread.h>    // For POSIX threads functions (pthread_create, pthread_join, pthread_mutex_init, etc.)
#include <time.h>       // Need time() for the helper function

// --- Global Variable ---

/**
 * @brief Global pointer to the application state.
 * @details This global pointer is necessary primarily for the signal handler (`handle_signal`).
 *          Signal handlers have a restricted signature (`void handler(int)`) and cannot easily
 *          be passed arbitrary context like the application state. Making the state globally
 *          accessible allows the signal handler to modify the `running` flag to initiate shutdown.
 *          It's initialized in `init_app_state`.
 * @warning Using global variables can make code harder to reason about and test. It's used
 *          here as a common pattern for simple signal handling in C.
 */
app_state_t *g_state = NULL;

// --- Function Definitions ---

/**
 * @brief Initializes the POSIX application state structure.
 */
void init_app_state(app_state_t *state, const char *username) {
    // Initialize the entire state structure to zeros.
    memset(state, 0, sizeof(app_state_t));

    // Set initial values for key state members.
    state->running = 1;
    state->tcp_socket = -1;
    state->udp_socket = -1;

    // Initialize the peer list using the shared function
    peer_shared_init_list(state->peers, MAX_PEERS);

    // Copy the provided username into the state structure's username field.
    strncpy(state->username, username, sizeof(state->username) - 1);
    state->username[sizeof(state->username) - 1] = '\0';

    // Initialize the mutex that will protect the `peers` array.
    pthread_mutex_init(&state->peers_mutex, NULL);

    // Set the global state pointer for the signal handler.
    g_state = state;

    // Set up signal handling for graceful shutdown.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/**
 * @brief Cleans up POSIX-specific resources.
 */
void cleanup_app_state(app_state_t *state) {
    log_message("Starting POSIX cleanup...");

    // Close sockets
    if (state->tcp_socket >= 0) {
        log_message("Closing TCP socket %d", state->tcp_socket);
        close(state->tcp_socket);
        state->tcp_socket = -1;
    }
    if (state->udp_socket >= 0) {
        log_message("Closing UDP socket %d", state->udp_socket);
        close(state->udp_socket);
        state->udp_socket = -1;
    }

    // Destroy the mutex.
    log_message("Destroying peers mutex");
    pthread_mutex_destroy(&state->peers_mutex);

    log_message("POSIX cleanup complete");
}

/**
 * @brief Adds/updates a peer (POSIX thread-safe wrapper).
 */
int add_peer(app_state_t *state, const char *ip, const char *username) {
    if (!state) return -1;
    pthread_mutex_lock(&state->peers_mutex); // Lock
    // Call the shared logic
    int result = peer_shared_add_or_update(state->peers, MAX_PEERS, ip, username);
    pthread_mutex_unlock(&state->peers_mutex); // Unlock
    return result;
}

/**
 * @brief Prunes timed-out peers (POSIX thread-safe wrapper).
 */
int prune_peers(app_state_t *state) {
    if (!state) return 0;
    pthread_mutex_lock(&state->peers_mutex); // Lock
    // Call the shared logic
    int count = peer_shared_prune_timed_out(state->peers, MAX_PEERS);
    pthread_mutex_unlock(&state->peers_mutex); // Unlock
    return count;
}


// --- Main Function (Remains the same as before) ---
int main(int argc, char *argv[]) {
    app_state_t state;
    pthread_t listener_tid, discovery_tid, input_tid;
    char username[32] = "anonymous";

    if (argc > 1) {
        strncpy(username, argv[1], sizeof(username) - 1);
        username[sizeof(username) - 1] = '\0';
    }

    init_app_state(&state, username);
    log_message("Starting P2P messaging application as '%s'", state.username);

    if (init_listener(&state) < 0) {
        log_message("Fatal: Failed to initialize TCP listener. Exiting.");
        cleanup_app_state(&state);
        return EXIT_FAILURE;
    }

    if (init_discovery(&state) < 0) {
        log_message("Fatal: Failed to initialize UDP discovery. Exiting.");
        cleanup_app_state(&state);
        return EXIT_FAILURE;
    }

    int listener_err = pthread_create(&listener_tid, NULL, listener_thread, &state);
    int discovery_err = pthread_create(&discovery_tid, NULL, discovery_thread, &state);
    int input_err = pthread_create(&input_tid, NULL, user_input_thread, &state);

    if (listener_err != 0 || discovery_err != 0 || input_err != 0) {
        log_message("Fatal: Failed to create one or more threads. Exiting.");
        state.running = 0;
        usleep(100000);
        // Attempt to join threads that might have started? Or just cleanup?
        // For simplicity, just cleanup state here. Proper handling might involve
        // trying to cancel/join successfully created threads.
        cleanup_app_state(&state);
        return EXIT_FAILURE;
    }

    log_message("Threads created successfully.");

    // Main thread now waits for user input thread to finish, then signals others
    pthread_join(input_tid, NULL);
    log_message("User input thread finished. Initiating shutdown...");

    state.running = 0; // Signal all threads to stop

    // Wait for other threads to complete
    log_message("Waiting for listener thread to finish...");
    pthread_join(listener_tid, NULL);
    log_message("Listener thread joined.");

    log_message("Waiting for discovery thread to finish...");
    pthread_join(discovery_tid, NULL);
    log_message("Discovery thread joined.");

    // Final cleanup
    cleanup_app_state(&state);
    log_message("Application terminated gracefully.");
    return EXIT_SUCCESS;
}