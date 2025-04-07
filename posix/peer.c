// Include the header file for this module, which defines the app_state_t
// and peer_t structures, constants, and declares the functions defined here.
#include "peer.h"
// Include headers for other modules providing necessary functionality.
#include "discovery.h"      // For init_discovery() and discovery_thread()
#include "messaging.h"      // For init_listener() and listener_thread()
#include "ui_terminal.h"    // For user_input_thread()
#include "signal_handler.h" // For handle_signal() function used with signal handling
#include "utils.h"          // For log_message() utility function

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

// --- Static Helper Function ---

/**
 * @brief Updates the timestamp and optionally the username of a peer entry.
 * @details Sets the peer's last_seen time to the current time and updates the username
 *          if a non-empty username is provided. This function is used to reduce code duplication
 *          between updating an existing peer and initializing a new peer entry.
 * @param peer Pointer to the peer_t structure to update.
 * @param username The potential new username (can be NULL or empty).
 * @note If username is NULL or empty, the existing username remains unchanged.
 *       This is important for updates where we may only want to refresh the timestamp.
 */
static void _update_peer_entry(peer_t *peer, const char *username) {
    if (!peer) return; // Basic null check to ensure peer is not NULL

    peer->last_seen = time(NULL); // Update the last_seen timestamp to the current time

    // Optionally update the username if a non-empty username is provided.
    if (username && username[0] != '\0') {
        strncpy(peer->username, username, sizeof(peer->username) - 1);
        // Ensure null termination after strncpy to prevent buffer overflow.
        peer->username[sizeof(peer->username) - 1] = '\0';
    }
    // If username is NULL or empty, the existing username remains unchanged.
}


// --- Function Definitions ---

/**
 * @brief Initializes the main application state structure.
 * @details Sets up the `app_state_t` struct passed by the caller. This involves:
 *          - Zeroing out the entire structure using `memset`.
 *          - Setting the `running` flag to 1 (true) initially.
 *          - Initializing socket descriptors (`tcp_socket`, `udp_socket`) to -1 (indicating they are not yet open).
 *          - Copying the provided `username` into the state structure safely using `strncpy`.
 *          - Initializing the `peers_mutex` for thread-safe access to the `peers` array.
 *          - Assigning the address of the state structure to the global `g_state` pointer for the signal handler.
 *          - Registering the `handle_signal` function to catch `SIGINT` (Ctrl+C) and `SIGTERM` (kill command) signals.
 * @param state Pointer to the `app_state_t` structure to be initialized.
 * @param username The desired username for this peer, provided as a command-line argument or defaulting to "anonymous".
 */
void init_app_state(app_state_t *state, const char *username) {
    // Initialize the entire state structure to zeros. This ensures all members
    // start with a known default value (0, NULL, etc.).
    memset(state, 0, sizeof(app_state_t));

    // Set initial values for key state members.
    state->running = 1;      // Application starts in a running state.
    state->tcp_socket = -1;  // TCP socket is not yet created/initialized.
    state->udp_socket = -1;  // UDP socket is not yet created/initialized.

    // Copy the provided username into the state structure's username field.
    // Use strncpy to prevent buffer overflows if the provided username is too long.
    // It copies at most `sizeof(state->username) - 1` characters and ensures
    // null-termination by placing '\0' at the last position.
    strncpy(state->username, username, sizeof(state->username) - 1);
    state->username[sizeof(state->username) - 1] = '\0'; // Ensure null termination

    // Initialize the mutex that will protect the `peers` array from race conditions
    // when accessed by multiple threads (discovery, listener, UI).
    pthread_mutex_init(&state->peers_mutex, NULL);

    // Set the global state pointer to point to the state structure we are initializing.
    // This allows the signal handler to access the application state.
    g_state = state;

    // Set up signal handling for graceful shutdown.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa)); // Initialize the sigaction struct.
    sa.sa_handler = handle_signal; // Specify the function to call when the signal occurs.
    // Register the handler for SIGINT (interrupt signal, typically Ctrl+C).
    sigaction(SIGINT, &sa, NULL); // Third argument NULL means we don't care about the old handler.
    // Register the handler for SIGTERM (termination signal, e.g., from `kill` command).
    sigaction(SIGTERM, &sa, NULL);
}

/**
 * @brief Cleans up resources allocated and managed by the application state.
 * @details This function is called during application shutdown to ensure proper
 *          release of system resources. It performs:
 *          - Closing the TCP listening socket if it's open (`>= 0`).
 *          - Closing the UDP discovery socket if it's open (`>= 0`).
 *          - Destroying the `peers_mutex` to release associated resources.
 *          - Logging each step of the cleanup process for debugging purposes.
 * @param state Pointer to the `app_state_t` structure containing the resources to clean up.
 * @warning This function, especially `pthread_mutex_destroy`, should only be called *after*
 *          all threads that might use these resources (sockets, mutex) have terminated or
 *          are guaranteed not to access them anymore. In this application, it's called
 *          in `main` after `pthread_join` ensures all threads have finished.
 */
void cleanup_app_state(app_state_t *state) {
    log_message("Starting cleanup..."); // Log the start of cleanup process

    // Close the TCP socket if it was successfully opened.
    if (state->tcp_socket >= 0) {
        log_message("Closing TCP socket %d", state->tcp_socket);
        close(state->tcp_socket); // Close the socket using the standard POSIX close() function
        state->tcp_socket = -1; // Mark as closed to prevent double-close issues
    }

    // Close the UDP socket if it was successfully opened.
    if (state->udp_socket >= 0) {
        log_message("Closing UDP socket %d", state->udp_socket);
        close(state->udp_socket); // Close the socket using the standard POSIX close() function
        state->udp_socket = -1; // Mark as closed to prevent double-close issues
    }

    // Destroy the mutex associated with the peers list.
    // This releases any resources held by the mutex object.
    log_message("Destroying peers mutex");
    pthread_mutex_destroy(&state->peers_mutex);

    log_message("Cleanup complete"); // Log the completion of cleanup process
}

/**
 * @brief Adds a new peer to the list or updates the last_seen time of an existing peer.
 * @details Provides thread-safe access to the shared `peers` array. Checks if a peer with the same IP exists,
 *          updates it if found, or adds a new peer if an empty slot is available.
 *          The function uses a helper function `_update_peer_entry` to avoid code duplication
 *          between updating existing peers and initializing new ones.
 * @param state Pointer to the application state containing the `peers` array and mutex.
 * @param ip The IP address string of the peer to add or update.
 * @param username The username string of the peer. Can be NULL or empty if only updating `last_seen`.
 * @return 1 if a new peer was successfully added to an empty slot.
 * @return 0 if an existing peer with the same IP was found and updated.
 * @return -1 if no existing peer was found and the peer list is full (no inactive slots available).
 */
int add_peer(app_state_t *state, const char *ip, const char *username) {
    pthread_mutex_lock(&state->peers_mutex); // Lock the mutex to ensure thread-safe access

    // --- Step 1: Check if the peer already exists ---
    for (int i = 0; i < MAX_PEERS; i++) {
        if (state->peers[i].active && strcmp(state->peers[i].ip, ip) == 0) {
            // Peer found. Update its details using the helper function.
            _update_peer_entry(&state->peers[i], username); // Use the refactored helper function

            pthread_mutex_unlock(&state->peers_mutex); // Unlock the mutex before returning
            return 0; // Indicate that an existing peer was updated.
        }
    }

    // --- Step 2: If peer doesn't exist, find an empty slot to add it ---
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!state->peers[i].active) {
            // Found an empty slot. Populate it with the new peer's data.
            strncpy(state->peers[i].ip, ip, INET_ADDRSTRLEN - 1);
            state->peers[i].ip[INET_ADDRSTRLEN - 1] = '\0'; // Ensure null termination
            state->peers[i].active = 1; // Mark the slot as active

            // Explicitly clear username before potential update in helper
            // This ensures we don't inherit a username from a previously inactive peer
            state->peers[i].username[0] = '\0';

            // Set timestamp and potentially username using the helper function.
            _update_peer_entry(&state->peers[i], username); // Use the refactored helper function

            pthread_mutex_unlock(&state->peers_mutex); // Unlock the mutex before returning
            return 1; // Indicate that a new peer was added.
        }
    }

    // --- Step 3: If loop finishes, no existing peer found and no empty slots available ---
    pthread_mutex_unlock(&state->peers_mutex); // Unlock the mutex before returning
    log_message("Peer list is full. Cannot add peer %s@%s.", username ? username : "??", ip);
    return -1; // Indicate that the list is full.
}

/**
 * @brief Main entry point of the P2P messaging application.
 * @details Orchestrates the application's lifecycle:
 *          1. Parses command-line arguments (expects optional username).
 *          2. Initializes the application state (`init_app_state`).
 *          3. Initializes network components (TCP listener, UDP discovery).
 *          4. Creates and starts the listener, discovery, and user input threads.
 *          5. Waits (`pthread_join`) for the user input thread to terminate (e.g., user types /quit).
 *          6. Sets the global `running` flag to 0 to signal other threads to stop gracefully.
 *          7. Waits (`pthread_join`) for the listener and discovery threads to finish.
 *          8. Cleans up resources (`cleanup_app_state`).
 *          9. Exits with success or failure status.
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line argument strings. `argv2` is potentially the username.
 * @return EXIT_SUCCESS (0) if the application runs and terminates normally.
 * @return EXIT_FAILURE (1) if initialization or thread creation fails.
 */
int main(int argc, char *argv[]) {
    // Structure to hold the application's state.
    app_state_t state;
    pthread_t listener_tid, discovery_tid, input_tid; // Thread identifiers for various functionalities
    char username[32] = "anonymous"; // Default username if none is provided

    // If a username is provided as a command-line argument, use it
    if (argc > 1) {
        strncpy(username, argv[1], sizeof(username) - 1);
        username[sizeof(username) - 1] = '\0'; // Ensure null termination
    }

    init_app_state(&state, username); // Initialize the application state
    log_message("Starting P2P messaging application as '%s'", state.username);

    // Initialize the TCP listener
    if (init_listener(&state) < 0) {
        log_message("Fatal: Failed to initialize TCP listener. Exiting.");
        cleanup_app_state(&state); // Attempt cleanup even on partial init failure
        return EXIT_FAILURE;
    }

    // Initialize the UDP discovery
    if (init_discovery(&state) < 0) {
        log_message("Fatal: Failed to initialize UDP discovery. Exiting.");
        cleanup_app_state(&state); // Attempt cleanup
        return EXIT_FAILURE;
    }

    // Create threads for listener, discovery, and user input
    int listener_err = pthread_create(&listener_tid, NULL, listener_thread, &state);
    int discovery_err = pthread_create(&discovery_tid, NULL, discovery_thread, &state);
    int input_err = pthread_create(&input_tid, NULL, user_input_thread, &state);

    // Check for thread creation errors
    if (listener_err != 0 || discovery_err != 0 || input_err != 0) {
        log_message("Fatal: Failed to create one or more threads. Exiting.");
        state.running = 0; // Signal any started threads to stop
        usleep(100000); // Brief delay to allow threads to notice the flag change
        cleanup_app_state(&state);
        return EXIT_FAILURE;
    }

    log_message("Threads created successfully.");
    pthread_join(input_tid, NULL); // Wait for the user input thread to finish (e.g., user types /quit)
    log_message("User input thread finished. Initiating shutdown...");
    state.running = 0; // Signal all threads to stop gracefully
    log_message("Waiting for listener thread to finish...");
    pthread_join(listener_tid, NULL); // Wait for the listener thread to finish
    log_message("Listener thread joined.");
    log_message("Waiting for discovery thread to finish...");
    pthread_join(discovery_tid, NULL); // Wait for the discovery thread to finish
    log_message("Discovery thread joined.");
    cleanup_app_state(&state); // Clean up application resources
    log_message("Application terminated gracefully.");
    return EXIT_SUCCESS; // Return success status
}