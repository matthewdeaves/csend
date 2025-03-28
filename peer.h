#ifndef PEER_H
#define PEER_H

#include <time.h>       // For time_t
#include <pthread.h>    // For pthread_mutex_t
#include <signal.h>     // For sig_atomic_t
#include <netinet/in.h> // For INET_ADDRSTRLEN (often included by arpa/inet.h)
#include <arpa/inet.h>  // For INET_ADDRSTRLEN

// --- Constants ---
#define PORT_TCP 8080
#define PORT_UDP 8081
#define BUFFER_SIZE 1024
#define MAX_PEERS 10
#define DISCOVERY_INTERVAL 10 // seconds
#define PEER_TIMEOUT 30 // seconds

// --- Data Structures ---

// Peer structure
typedef struct {
    char ip[INET_ADDRSTRLEN]; // Requires <arpa/inet.h> or <netinet/in.h>
    char username[32];
    time_t last_seen;         // Requires <time.h>
    int active;
} peer_t;

// Application state structure
typedef struct app_state_t {
    volatile sig_atomic_t running;  // Flag indicating if the application is running; volatile ensures visibility across threads
                                    // and sig_atomic_t guarantees atomic access from signal handlers. 
                                    // Requires <signal.h>

    peer_t peers[MAX_PEERS];        // Array of peer structures containing information about connected peers
                                    // Limited to MAX_PEERS entries to prevent unbounded memory usage
    
    int tcp_socket;                 // File descriptor for the TCP socket used for reliable message exchange
                                    // Negative value indicates an uninitialized or closed socket
    
    int udp_socket;                 // File descriptor for the UDP socket used for peer discovery broadcasts
                                    // Negative value indicates an uninitialized or closed socket
    
    char username[32];              // User's chosen display name, limited to 31 characters plus null terminator
                                    // Used to identify this peer in messages to others
    
    pthread_mutex_t peers_mutex;    // Mutex to protect concurrent access to the peers array
                                    // Ensures thread safety when multiple threads read/write peer information
                                    // Basically means r/w to this var can not be interrupted. Threads call for to
                                    // hold the lock on this var and will be blocked until the lock is released by another
                                    // Requires <pthread.h>
} app_state_t;


// --- Function Declarations (defined in peer.c) ---

/**
 * @brief Initializes the application state structure.
 * @param state Pointer to the app_state_t structure to initialize.
 * @param username The username for this peer.
 */
void init_app_state(app_state_t *state, const char *username);

/**
 * @brief Cleans up resources associated with the application state.
 * Closes sockets and destroys the mutex.
 * @param state Pointer to the app_state_t structure to clean up.
 */
void cleanup_app_state(app_state_t *state);

/**
 * @brief Adds a new peer to the list or updates an existing one.
 * This function is thread-safe.
 * @param state Pointer to the application state.
 * @param ip IP address of the peer.
 * @param username Username of the peer.
 * @return 1 if a new peer was added, 0 if an existing peer was updated, -1 if the list is full.
 */
int add_peer(app_state_t *state, const char *ip, const char *username);


// --- Global State ---

// Global pointer to the application state, primarily for the signal handler.
extern app_state_t *g_state;

#endif // PEER_H