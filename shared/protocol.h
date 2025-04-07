// Include guard: Prevents the header file from being included multiple times
// in the same compilation unit, which would cause errors due to redefinitions.
#ifndef PROTOCOL_H // If PROTOCOL_H is not defined...
#define PROTOCOL_H // ...define PROTOCOL_H.

// Include common definitions needed by the protocol functions and callers.
// - BUFFER_SIZE: Defines the expected maximum size for message content and temporary buffers used in protocol.c.
// - INET_ADDRSTRLEN: Defines the necessary size for buffers holding IPv4 address strings (used in parse_message output).
#include "common_defs.h"

// --- Message Type Constants ---
// Define string constants representing the different types of messages used in the protocol.
// These are used as the first part of the formatted message string ("TYPE|...").

// Message type used for broadcasting to discover other peers on the network.
#define MSG_DISCOVERY "DISCOVERY"
// Message type used to respond to a received MSG_DISCOVERY.
#define MSG_DISCOVERY_RESPONSE "DISCOVERY_RESPONSE"
// Message type for regular text chat messages between peers.
#define MSG_TEXT "TEXT"
// Message type sent by a peer just before it shuts down gracefully.
#define MSG_QUIT "QUIT"

// --- Function Declarations ---
// These declare the functions implemented in protocol.c, making them available to other modules.

/**
 * @brief Formats message components into a single string according to the application protocol.
 * @details Constructs a string following the format: "TYPE|SENDER@IP|CONTENT".
 *          The caller must provide the local IP address to be embedded.
 *          This function is used before sending any message (discovery, text, quit) over the network.
 * @param buffer Pointer to the character buffer where the resulting formatted string will be written.
 *               The caller must provide a buffer large enough.
 * @param buffer_size The total size (in bytes) of the `buffer`.
 * @param msg_type A string representing the message type (e.g., MSG_TEXT).
 * @param sender The username of the peer sending the message.
 * @param local_ip_str A string containing the local IP address of the sender (e.g., "192.168.1.10").
 *                     If NULL or empty, "unknown" will be used.
 * @param content The payload or content of the message.
 * @return 0 on successful formatting.
 * @return -1 if the formatted message would exceed `buffer_size` or on encoding error.
 */
int format_message(char *buffer, int buffer_size, const char *msg_type,
    const char *sender, const char *local_ip_str, const char *content); // Added local_ip_str

/**
 * @brief Parses an incoming message string received from the network into its components.
 * @details Deconstructs a string assumed to be in the format "TYPE|SENDER@IP|CONTENT".
 *          It extracts the message type, the sender's username, the sender's IP address,
 *          and the message content into separate output buffers provided by the caller.
 * @param buffer A pointer to the constant character buffer containing the raw message string received.
 * @param sender_ip Pointer to an output character buffer (allocated by caller) where the extracted
 *                  sender IP address string will be stored. **Caller must ensure this buffer is at least `INET_ADDRSTRLEN` bytes.**
 * @param sender_username Pointer to an output character buffer (allocated by caller) where the extracted
 *                        sender username string will be stored. **Caller must ensure this buffer is at least 32 bytes.**
 * @param msg_type Pointer to an output character buffer (allocated by caller) where the extracted
 *                 message type string will be stored. **Caller must ensure this buffer is at least 32 bytes.**
 * @param content Pointer to an output character buffer (allocated by caller) where the extracted
 *                message content string will be stored. **Caller must ensure this buffer is large enough for expected content (e.g., `BUFFER_SIZE` bytes).**
 * @return 0 on successful parsing of all components according to the expected format.
 * @return -1 if the input `buffer` does not conform to the "TYPE|SENDER@IP|CONTENT" structure
 *         (e.g., missing '|' delimiters), indicating a parsing failure. Output buffers may contain partial or invalid data on failure.
 */
int parse_message(const char *buffer, char *sender_ip, char *sender_username,
                 char *msg_type, char *content);

#endif // End of the include guard PROTOCOL_H