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
// Moved from peer.h as they are fundamental to the protocol

// Defines the TCP port number used for listening for incoming peer connections and sending messages.
#define PORT_TCP 8080
// Defines the UDP port number used for peer discovery broadcasts and responses.
#define PORT_UDP 8081


#endif // COMMON_DEFS_H