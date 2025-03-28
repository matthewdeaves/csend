// Include the header file for this module, which defines the app_state_t
// and peer_t structures, constants, and declares the functions defined here.
#include "peer.h"
// Include headers for other modules providing necessary functionality.
#include "discovery.h"      // For init_discovery() and discovery_thread()
#include "messaging.h"        // For init_listener() and listener_thread()
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
    // The second argument `NULL` specifies default mutex attributes.
    // Error checking for pthread_mutex_init is omitted for brevity but recommended in production code.
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
    // Note: Error checking for sigaction is omitted.
}

/**
 * @brief Cleans up resources allocated and managed by the application state.
 * @details This function is called during application shutdown to ensure proper
 *          release of system resources. It performs:
 *          - Closing the TCP listening socket if it's open (`>= 0`).
 *          - Closing the UDP discovery socket if it's open (`>= 0`).
 *          - Destroying the `peers_mutex` to release associated resources.
 * @param state Pointer to the `app_state_t` structure containing the resources to clean up.
 * @warning This function, especially `pthread_mutex_destroy`, should only be called *after*
 *          all threads that might use these resources (sockets, mutex) have terminated or
 *          are guaranteed not to access them anymore. In this application, it's called
 *          in `main` after `pthread_join` ensures all threads have finished.
 */
void cleanup_app_state(app_state_t *state) {
    log_message("Starting cleanup...");
    // Close the TCP socket if it was successfully opened.
    if (state->tcp_socket >= 0) {
        log_message("Closing TCP socket %d", state->tcp_socket);
        close(state->tcp_socket);
        state->tcp_socket = -1; // Mark as closed.
    }

    // Close the UDP socket if it was successfully opened.
    if (state->udp_socket >= 0) {
        log_message("Closing UDP socket %d", state->udp_socket);
        close(state->udp_socket);
        state->udp_socket = -1; // Mark as closed.
    }

    // Destroy the mutex associated with the peers list.
    // This releases any resources held by the mutex object.
    // It's crucial that no thread attempts to lock/unlock this mutex after it's destroyed.
    log_message("Destroying peers mutex");
    pthread_mutex_destroy(&state->peers_mutex);
    // Note: Error checking for close() and pthread_mutex_destroy() is omitted.

    log_message("Cleanup complete");
}

/**
 * @brief Adds a new peer to the list or updates the last_seen time of an existing peer.
 * @details This function provides thread-safe access to the shared `peers` array.
 *          It first checks if a peer with the same IP address already exists.
 *          - If found and active, it updates the `last_seen` timestamp and optionally the username.
 *          - If not found, it looks for an inactive (`active == 0`) slot in the array.
 *          - If an empty slot is found, it populates it with the new peer's information
 *            (IP, username, current time as `last_seen`) and marks it as active.
 *          The entire operation is protected by locking and unlocking `state->peers_mutex`.
 * @param state Pointer to the application state containing the `peers` array and mutex.
 * @param ip The IP address string of the peer to add or update.
 * @param username The username string of the peer. Can be NULL or empty if only updating `last_seen`.
 * @return 1 if a new peer was successfully added to an empty slot.
 * @return 0 if an existing peer with the same IP was found and updated.
 * @return -1 if no existing peer was found and the peer list is full (no inactive slots available).
 */
int add_peer(app_state_t *state, const char *ip, const char *username) {
    // Acquire the lock on the peers mutex before accessing the shared peers array.
    // This prevents race conditions if multiple threads call add_peer concurrently.
    // If the mutex is already held by another thread, this call will block until it's released.
    pthread_mutex_lock(&state->peers_mutex);

    // --- Step 1: Check if the peer already exists ---
    for (int i = 0; i < MAX_PEERS; i++) {
        // Check if the current slot 'i' is active and the IP address matches.
        if (state->peers[i].active && strcmp(state->peers[i].ip, ip) == 0) {
            // Peer found. Update its last_seen time to the current time.
            state->peers[i].last_seen = time(NULL);

            // Optionally update the username if a non-empty username is provided.
            // This allows discovery responses/messages to update usernames if they changed.
            if (username && username[0] != '\0') {
                strncpy(state->peers[i].username, username, sizeof(state->peers[i].username) - 1);
                // Ensure null termination after strncpy.
                state->peers[i].username[sizeof(state->peers[i].username) - 1] = '\0';
            }

            // Release the mutex lock as we are done modifying the shared data.
            pthread_mutex_unlock(&state->peers_mutex);
            return 0; // Indicate that an existing peer was updated.
        }
    }

    // --- Step 2: If peer doesn't exist, find an empty slot to add it ---
    for (int i = 0; i < MAX_PEERS; i++) {
        // Check if the current slot 'i' is inactive (available).
        if (!state->peers[i].active) {
            // Found an empty slot. Populate it with the new peer's data.
            // Copy the IP address safely.
            strncpy(state->peers[i].ip, ip, INET_ADDRSTRLEN - 1);
            state->peers[i].ip[INET_ADDRSTRLEN - 1] = '\0'; // Ensure null termination

            // Set the last_seen time to the current time.
            state->peers[i].last_seen = time(NULL);
            // Mark the slot as active.
            state->peers[i].active = 1;

            // Set the username, ensuring null termination.
            // If username is NULL or empty, set a default or leave it empty (depends on desired behavior).
            // Here, we copy if provided, otherwise it might remain from a previous inactive peer.
            // It might be better to explicitly set to "unknown" or empty if username is NULL/empty.
            if (username && username[0] != '\0') {
                strncpy(state->peers[i].username, username, sizeof(state->peers[i].username) - 1);
                state->peers[i].username[sizeof(state->peers[i].username) - 1] = '\0';
            } else {
                // Explicitly set to empty if no username provided for a new entry
                 state->peers[i].username[0] = '\0';
            }


            // Release the mutex lock.
            pthread_mutex_unlock(&state->peers_mutex);
            return 1; // Indicate that a new peer was added.
        }
    }

    // --- Step 3: If loop finishes, no existing peer found and no empty slots available ---
    // Release the mutex lock.
    pthread_mutex_unlock(&state->peers_mutex);
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
 * @param argv Array of command-line argument strings. `argv[1]` is potentially the username.
 * @return EXIT_SUCCESS (0) if the application runs and terminates normally.
 * @return EXIT_FAILURE (1) if initialization or thread creation fails.
 */
int main(int argc, char *argv[]) {
    // Structure to hold the application's state.
    app_state_t state;
    // Thread identifiers for the created threads.
    pthread_t listener_tid, discovery_tid, input_tid;
    // Default username if none is provided via command line.
    char username[32] = "anonymous";

    // Check if a username was provided as the first command-line argument.
    if (argc > 1) {
        // Copy the provided username safely into the username buffer.
        strncpy(username, argv[1], sizeof(username) - 1);
        username[sizeof(username) - 1] = '\0'; // Ensure null termination.
    }

    // Initialize the application state structure, including signal handlers.
    init_app_state(&state, username);

    log_message("Starting P2P messaging application as '%s'", state.username);

    // Initialize the network components (TCP listener and UDP discovery sockets).
    // If either initialization fails, log the error, clean up, and exit.
    if (init_listener(&state) < 0) {
        log_message("Fatal: Failed to initialize TCP listener. Exiting.");
        cleanup_app_state(&state); // Attempt cleanup even on partial init failure.
        return EXIT_FAILURE;
    }
     if (init_discovery(&state) < 0) {
        log_message("Fatal: Failed to initialize UDP discovery. Exiting.");
        cleanup_app_state(&state); // Attempt cleanup.
        return EXIT_FAILURE;
    }


    // Start the worker threads.
    // Pass the address of the state structure as the argument to each thread function.
    // Error checking for pthread_create is important.
    int listener_err = pthread_create(&listener_tid, NULL, listener_thread, &state);
    int discovery_err = pthread_create(&discovery_tid, NULL, discovery_thread, &state);
    int input_err = pthread_create(&input_tid, NULL, user_input_thread, &state);

    // Check if any thread creation failed.
    if (listener_err != 0 || discovery_err != 0 || input_err != 0) {
        log_message("Fatal: Failed to create one or more threads. Exiting.");
        // If thread creation fails, signal any potentially started threads to stop.
        state.running = 0;

        // Give potentially started threads a brief moment to notice the flag change
        // before we destroy the mutex in cleanup_app_state. This is a pragmatic
        // approach; more robust error handling might involve trying to join started threads.
        usleep(100000); // 100ms delay.

        // Clean up resources.
        cleanup_app_state(&state);
        return EXIT_FAILURE;
    }

    log_message("Threads created successfully.");

    // Wait for the user input thread to complete.
    // This thread will likely exit when the user enters the "/quit" command,
    // which sets state.running = 0 and returns from the thread function.
    // pthread_join blocks the main thread until input_tid finishes.
    pthread_join(input_tid, NULL); // Second argument NULL means we don't care about the thread's return value.

    log_message("User input thread finished. Initiating shutdown...");

    // Signal the other threads (listener, discovery) to stop by setting the running flag to 0.
    // These threads periodically check this flag in their main loops.
    // Note: The user input thread might have already set this flag if /quit was used. Setting it again is harmless.
    state.running = 0;

    // Wait for the listener and discovery threads to terminate gracefully.
    // pthread_join ensures that the main thread doesn't proceed until these threads
    // have exited their loops and returned. This is crucial before cleanup.
    log_message("Waiting for listener thread to finish...");
    pthread_join(listener_tid, NULL);
    log_message("Listener thread joined.");

    log_message("Waiting for discovery thread to finish...");
    pthread_join(discovery_tid, NULL);
    log_message("Discovery thread joined.");

    // All threads have finished. Now it's safe to clean up shared resources.
    // This closes sockets and destroys the mutex.
    cleanup_app_state(&state);

    log_message("Application terminated gracefully.");
    // Return success status.
    return EXIT_SUCCESS;
}