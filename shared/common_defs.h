// FILE: ./shared/common_defs.h
#ifndef COMMON_DEFS_H
#define COMMON_DEFS_H

// --- Buffer and Network Constants ---

// Defines the standard size (in bytes) for message buffers used for sending and receiving data.
#define BUFFER_SIZE 1024

// Defines the maximum length of a standard IPv4 address string (e.g., "xxx.xxx.xxx.xxx")
// including the null terminator. This value is standard across POSIX systems.
// We define it here for use in shared code without needing POSIX headers directly.
// Standard value is 16.
#define INET_ADDRSTRLEN 16

// --- Port Constants ---

// Defines the TCP port number used for listening for incoming peer connections and sending messages.
#define PORT_TCP 8080
// Defines the UDP port number used for peer discovery broadcasts and responses.
#define PORT_UDP 8081

// --- Peer Constants ---

// Defines the maximum number of peers that can be stored in the application's peer list.
#define MAX_PEERS 10
// Defines the interval (in seconds) between sending UDP discovery broadcast messages.
#define DISCOVERY_INTERVAL 10 // Seconds
// Defines the duration (in seconds) after which an inactive peer is considered timed out and removed/ignored.
#define PEER_TIMEOUT 30       // Seconds

// --- Peer Data Structure ---
/**
 * @brief Structure representing a known peer in the network.
 * @details Holds information about another peer discovered or connected to.
 *          Intended for use in both POSIX and Classic Mac builds.
 */
typedef struct {
    /**
     * @brief The IPv4 address of the peer as a string (e.g., "192.168.1.10").
     * @details Size INET_ADDRSTRLEN includes null terminator.
     */
    char ip[INET_ADDRSTRLEN];

    /**
     * @brief The username chosen by the peer.
     * @details Limited to 31 characters plus the null terminator.
     */
    char username[32];

    /**
     * @brief Timestamp indicating when the peer was last heard from.
     * @details Platform-dependent interpretation:
     *          - POSIX: Stores time_t (seconds since epoch), cast to unsigned long.
     *          - Classic Mac: Stores TickCount() (system ticks, approx 1/60th sec).
     *          Timeout comparison logic must account for this difference.
     */
    unsigned long last_seen;

    /**
     * @brief Flag indicating if this peer entry is currently considered active.
     * @details 0 = inactive/timed out/empty slot, 1 = active.
     */
    int active;
} peer_t;


#endif // COMMON_DEFS_H