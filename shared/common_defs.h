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
#define DISCOVERY_INTERVAL 10
// Defines the duration (in seconds) after which an inactive peer is considered timed out and removed/ignored.
#define PEER_TIMEOUT 30


#endif // COMMON_DEFS_H