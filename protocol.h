#ifndef PROTOCOL_H
#define PROTOCOL_H

// Include peer.h for constants like BUFFER_SIZE, INET_ADDRSTRLEN
// which implicitly define buffer sizes used/expected by these functions.
#include "peer.h"

// --- Message Types
#define MSG_DISCOVERY "DISCOVERY"
#define MSG_DISCOVERY_RESPONSE "DISCOVERY_RESPONSE"
#define MSG_TEXT "TEXT"
#define MSG_QUIT "QUIT"

/**
 * @brief Formats a message according to the application protocol.
 * Format: TYPE|SENDER@IP|CONTENT
 * Note: Automatically retrieves the local IP to include in the sender field.
 * @param buffer Output buffer to store the formatted message.
 * @param buffer_size Size of the output buffer.
 * @param msg_type The type of the message (e.g., MSG_TEXT).
 * @param sender The username of the sender.
 * @param content The message content.
 * @return 0 on success, -1 if the buffer is too small.
 */
int format_message(char *buffer, int buffer_size, const char *msg_type,
                  const char *sender, const char *content);

/**
 * @brief Parses an incoming message string according to the application protocol.
 * Extracts type, sender username, sender IP, and content.
 * Format: TYPE|SENDER@IP|CONTENT
 * @param buffer The input message buffer.
 * @param sender_ip Output buffer to store the extracted sender IP address (should be at least INET_ADDRSTRLEN).
 * @param sender_username Output buffer to store the extracted sender username (should be at least 32 chars).
 * @param msg_type Output buffer to store the extracted message type (should be at least 32 chars).
 * @param content Output buffer to store the extracted message content (should be at least BUFFER_SIZE).
 * @return 0 on success, -1 on parsing failure (invalid format).
 */
int parse_message(const char *buffer, char *sender_ip, char *sender_username,
                 char *msg_type, char *content);

#endif // PROTOCOL_H