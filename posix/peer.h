// Include guard: Prevents the header file from being included multiple times
// in a single compilation unit, avoiding redefinition errors.
#ifndef PEER_H // If the symbol PEER_H is not defined...
#define PEER_H // ...then define it.

// --- Standard & System Library Includes ---

// Provides definitions for time-related types and functions, specifically `time_t`
// used in the peer_t struct to track when a peer was last seen.
#include <time.h>
// Provides definitions for POSIX threads (Pthreads) types and functions,
// specifically `pthread_mutex_t` used for thread synchronization (protecting the peer list).
#include <pthread.h>
// Provides definitions related to signal handling, specifically `sig_atomic_t`
// used for the 'running' flag to ensure atomic access from signal handlers.
#include <signal.h>

// --- Project Includes ---
#include "common_defs.h" // Include the new shared definitions

// --- Data Structures ---

/**
 * @brief Structure holding the overall state of the running application instance.
 * @details Contains all the essential information shared across different threads
 *          and modules of the application, such as network sockets, peer list,
 *          and control flags.
 */
typedef struct app_state_t {
    /**
     * @brief Flag indicating whether the application should continue running.
     * @details Set to 1 initially, set to 0 to signal all threads to terminate gracefully.
     *          `volatile`: Tells the compiler that this variable's value can change unexpectedly
     *                      (e.g., by a signal handler or another thread), preventing certain optimizations
     *                      that might assume the value doesn't change. Ensures reads always fetch the current value.
     *          `sig_atomic_t`: Guarantees that read and write operations on this variable are atomic
     *                          with respect to signal handlers. This means a signal handler can safely
     *                          write to it without interrupting a read/write in the main code, preventing data corruption.
     *                          Requires including <signal.h>.
     */
    volatile sig_atomic_t running;

    /**
     * @brief Fixed-size array holding information about known peers.
     * @details Stores `peer_t` structures for up to `MAX_PEERS`. Access to this array
     *          (adding, updating, reading) must be synchronized using `peers_mutex`
     *          to prevent race conditions between threads.
     */
    peer_t peers[MAX_PEERS];

    /**
     * @brief File descriptor for the main TCP listening socket.
     * @details Used by the listener thread to `accept` incoming connections from other peers
     *          on `PORT_TCP`. Initialized to -1 and assigned a valid descriptor by `init_listener`.
     */
    int tcp_socket;

    /**
     * @brief File descriptor for the UDP socket used for discovery.
     * @details Used by the discovery thread to send broadcast messages and receive discovery
     *          responses/requests on `PORT_UDP`. Initialized to -1 and assigned a valid descriptor
     *          by `init_discovery`.
     */
    int udp_socket;

    /**
     * @brief The username chosen for this instance of the application.
     * @details Used when sending messages or discovery responses to identify this peer.
     *          Limited to 31 characters plus the null terminator.
     */
    char username[32];

    /**
     * @brief Mutex (Mutual Exclusion object) for protecting the `peers` array.
     * @details Before any thread reads from or writes to the `peers` array, it must
     *          lock this mutex using `pthread_mutex_lock()`. After finishing the access,
     *          it must unlock the mutex using `pthread_mutex_unlock()`. This ensures that
     *          only one thread can access the `peers` array at any given time, preventing
     *          data corruption and race conditions. Requires including <pthread.h>.
     */
    pthread_mutex_t peers_mutex;
} app_state_t;


// --- Function Declarations (defined in peer.c) ---
// These declare functions implemented in peer.c, making them callable from other modules.

/**
 * @brief Initializes the application state structure (`app_state_t`).
 * @details Sets default values, initializes the mutex, copies the username,
 *          sets up signal handlers, and assigns the global state pointer (`g_state`).
 * @param state Pointer to the `app_state_t` structure (allocated by the caller, typically `main`) to initialize.
 * @param username The username chosen for this peer instance.
 */
void init_app_state(app_state_t *state, const char *username);

/**
 * @brief Cleans up resources associated with the application state before exiting.
 * @details Closes the TCP and UDP sockets if they are open and destroys the `peers_mutex`.
 *          Should be called after all threads have terminated.
 * @param state Pointer to the `app_state_t` structure whose resources need cleaning up.
 */
void cleanup_app_state(app_state_t *state);

/**
 * @brief Adds a new peer to the list or updates an existing one based on IP address.
 * @details This function handles the logic of maintaining the peer list in a thread-safe manner.
 *          It checks if the peer exists, updates `last_seen` if it does, or finds an empty
 *          slot and adds the new peer if it doesn't exist and space is available.
 *          Uses `peers_mutex` internally for synchronization.
 * @param state Pointer to the application state containing the peer list and mutex.
 * @param ip The IP address string of the peer to add/update.
 * @param username The username string of the peer (can be NULL or empty if only updating).
 * @return 1 if a *new* peer was successfully added.
 * @return 0 if an *existing* peer was found and updated.
 * @return -1 if the peer list is full and the new peer could not be added.
 */
int add_peer(app_state_t *state, const char *ip, const char *username);

/**
 * @brief Prunes timed-out peers (POSIX thread-safe wrapper).
 * @details Locks the mutex, calls peer_shared_prune_timed_out, unlocks mutex.
 * @return Number of peers pruned.
 */
int prune_peers(app_state_t *state);


// --- Global State ---

/**
 * @brief Declaration of a global pointer to the application state.
 * @details The `extern` keyword indicates that this variable is *defined* elsewhere
 *          (in peer.c) but can be accessed by any module that includes this header.
 *          Its primary purpose is to allow the signal handler (`handle_signal`, typically
 *          defined in signal_handler.c but needs access to the state) to modify the
 *          `running` flag to initiate a graceful shutdown.
 */
extern app_state_t *g_state;

#endif // End of the include guard PEER_H