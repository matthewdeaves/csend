#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "common_defs.h"

#ifdef __MACOS__
#include <MacTypes.h>
typedef UInt32 csend_uint32_t;
#else
#include <stdint.h>
typedef uint32_t csend_uint32_t;
#endif

/* Protocol magic number - "CSDC" in ASCII */
#define MSG_MAGIC_NUMBER 0x43534443UL

/* Protocol message types */
#define MSG_DISCOVERY          "DISCOVERY"
#define MSG_DISCOVERY_RESPONSE "DISCOVERY_RESPONSE"
#define MSG_TEXT              "TEXT"
#define MSG_QUIT              "QUIT"

/* Protocol field size limits */
#define PROTOCOL_MAX_MSG_TYPE_LEN    31
#define PROTOCOL_MAX_USERNAME_LEN    31
#define PROTOCOL_MAX_IP_LEN         (INET_ADDRSTRLEN - 1)
#define PROTOCOL_MAX_CONTENT_LEN    (BUFFER_SIZE - 1)

/* Minimum sizes for protocol operations */
#define PROTOCOL_MIN_MESSAGE_SIZE   (sizeof(csend_uint32_t) + 3)  /* magic + "||" */
#define PROTOCOL_MIN_BUFFER_SIZE    64  /* Reasonable minimum for any message */

/* Protocol format: [4-byte magic][msg_type]|[msg_id]|[sender@ip]|[content] */

/**
 * generate_message_id - Generate a unique message ID
 *
 * Generates a unique message ID for each message sent. The ID is a simple
 * monotonically increasing counter that wraps at UINT32_MAX. This is useful
 * for message tracking, deduplication, and ordering independent of clock sync.
 *
 * This function is thread-safe on platforms that support atomic operations.
 *
 * Return: A unique message ID
 */
csend_uint32_t generate_message_id(void);

/**
 * format_message - Format a message for network transmission
 * @buffer: Output buffer to write formatted message
 * @buffer_size: Size of output buffer in bytes
 * @msg_type: Message type (e.g., MSG_TEXT, MSG_QUIT)
 * @msg_id: Message ID (use generate_message_id() to create)
 * @sender: Sender's username (NULL defaults to "anon")
 * @local_ip_str: Sender's IP address (NULL defaults to "unknown")
 * @content: Message content (NULL defaults to empty string)
 *
 * Formats a message according to the protocol specification.
 * The output includes a 4-byte magic number followed by pipe-delimited fields.
 *
 * Return: Total bytes written including null terminator on success, 0 on error
 */
int format_message(char *buffer, int buffer_size, const char *msg_type,
                   csend_uint32_t msg_id, const char *sender, const char *local_ip_str,
                   const char *content);

/**
 * parse_message - Parse a received network message
 * @buffer: Input buffer containing the message
 * @buffer_len: Length of input buffer in bytes
 * @sender_ip: Output buffer for sender's IP (size >= INET_ADDRSTRLEN)
 * @sender_username: Output buffer for sender's username (size >= 32)
 * @msg_type: Output buffer for message type (size >= 32)
 * @msg_id: Output pointer for message ID (NULL if not needed)
 * @content: Output buffer for message content (size >= BUFFER_SIZE)
 *
 * Parses a message formatted according to the protocol specification.
 * All output parameters are optional (can be NULL).
 * Output buffers are always null-terminated, even on error.
 *
 * Return: 0 on success, -1 on error
 */
int parse_message(const char *buffer, int buffer_len, char *sender_ip, char *sender_username,
                  char *msg_type, csend_uint32_t *msg_id, char *content);

/* Protocol validation: Use strcmp() for message type comparison */

/**
 * PROTOCOL_OVERHEAD - Get the protocol overhead in bytes
 * (magic number + delimiters + null terminator)
 */
#define PROTOCOL_OVERHEAD (sizeof(csend_uint32_t) + 3)

#endif /* PROTOCOL_H */