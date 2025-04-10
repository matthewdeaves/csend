// FILE: ./shared/common_defs.h
#ifndef COMMON_DEFS_H
#define COMMON_DEFS_H

// --- Time/System Includes (Conditionally) ---
#ifdef __POSIX__
#include <time.h> // Include time.h ONLY for POSIX builds to define time_t
#endif

// --- Buffer and Network Constants ---
#define BUFFER_SIZE 1024
#define INET_ADDRSTRLEN 16

// --- Port Constants ---
#define PORT_TCP 8080
#define PORT_UDP 8081

// --- Peer Constants ---
#define MAX_PEERS 10
#define DISCOVERY_INTERVAL 10 // In seconds
#define PEER_TIMEOUT 30       // In seconds

// --- Peer Data Structure ---
/**
 * @brief Structure representing a known peer in the network.
 * @details Holds information about another peer discovered or connected to.
 *          Classic Mac version uses 'last_seen_ticks' instead of time_t.
 */
typedef struct {
    /**
     * @brief The IPv4 address of the peer as a string.
     */
    char ip[INET_ADDRSTRLEN];

    /**
     * @brief The username chosen by the peer.
     */
    char username[32];

#ifdef __POSIX__ // Use time_t for POSIX
    /**
     * @brief The timestamp (seconds since epoch) when the peer was last heard from. (POSIX)
     */
    time_t last_seen; // time_t is now defined via the include above
#else // Use Ticks for Classic Mac
    /**
     * @brief The timestamp (in system Ticks) when the peer was last heard from. (Classic Mac)
     */
    unsigned long last_seen_ticks;
#endif

    /**
     * @brief Flag indicating if this peer entry is currently considered active.
     */
    int active;
} peer_t;


#endif // COMMON_DEFS_H