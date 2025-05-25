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

/* Protocol format: [4-byte magic][msg_type]|[sender@ip]|[content] */

/**
 * format_message - Format a message for network transmission
 * @buffer: Output buffer to write formatted message
 * @buffer_size: Size of output buffer in bytes
 * @msg_type: Message type (e.g., MSG_TEXT, MSG_QUIT)
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
                   const char *sender, const char *local_ip_str, const char *content);

/**
 * parse_message - Parse a received network message
 * @buffer: Input buffer containing the message
 * @buffer_len: Length of input buffer in bytes
 * @sender_ip: Output buffer for sender's IP (size >= INET_ADDRSTRLEN)
 * @sender_username: Output buffer for sender's username (size >= 32)
 * @msg_type: Output buffer for message type (size >= 32)
 * @content: Output buffer for message content (size >= BUFFER_SIZE)
 *
 * Parses a message formatted according to the protocol specification.
 * All output parameters are optional (can be NULL).
 * Output buffers are always null-terminated, even on error.
 *
 * Return: 0 on success, -1 on error
 */
int parse_message(const char *buffer, int buffer_len, char *sender_ip, char *sender_username,
                  char *msg_type, char *content);

/* Protocol validation macros (no dependencies) */

/**
 * IS_VALID_MSG_DISCOVERY - Check if string matches DISCOVERY message type
 */
#define IS_VALID_MSG_DISCOVERY(msg) \
    ((msg) != NULL && (msg)[0] == 'D' && (msg)[1] == 'I' && (msg)[2] == 'S' && \
     (msg)[3] == 'C' && (msg)[4] == 'O' && (msg)[5] == 'V' && (msg)[6] == 'E' && \
     (msg)[7] == 'R' && (msg)[8] == 'Y' && (msg)[9] == '\0')

/**
 * IS_VALID_MSG_DISCOVERY_RESPONSE - Check if string matches DISCOVERY_RESPONSE
 */
#define IS_VALID_MSG_DISCOVERY_RESPONSE(msg) \
    ((msg) != NULL && (msg)[0] == 'D' && (msg)[1] == 'I' && (msg)[2] == 'S' && \
     (msg)[3] == 'C' && (msg)[4] == 'O' && (msg)[5] == 'V' && (msg)[6] == 'E' && \
     (msg)[7] == 'R' && (msg)[8] == 'Y' && (msg)[9] == '_' && (msg)[10] == 'R' && \
     (msg)[11] == 'E' && (msg)[12] == 'S' && (msg)[13] == 'P' && (msg)[14] == 'O' && \
     (msg)[15] == 'N' && (msg)[16] == 'S' && (msg)[17] == 'E' && (msg)[18] == '\0')

/**
 * IS_VALID_MSG_TEXT - Check if string matches TEXT message type
 */
#define IS_VALID_MSG_TEXT(msg) \
    ((msg) != NULL && (msg)[0] == 'T' && (msg)[1] == 'E' && \
     (msg)[2] == 'X' && (msg)[3] == 'T' && (msg)[4] == '\0')

/**
 * IS_VALID_MSG_QUIT - Check if string matches QUIT message type
 */
#define IS_VALID_MSG_QUIT(msg) \
    ((msg) != NULL && (msg)[0] == 'Q' && (msg)[1] == 'U' && \
     (msg)[2] == 'I' && (msg)[3] == 'T' && (msg)[4] == '\0')

/**
 * IS_VALID_MESSAGE_TYPE - Check if a string is any valid message type
 */
#define IS_VALID_MESSAGE_TYPE(msg) \
    (IS_VALID_MSG_DISCOVERY(msg) || \
     IS_VALID_MSG_DISCOVERY_RESPONSE(msg) || \
     IS_VALID_MSG_TEXT(msg) || \
     IS_VALID_MSG_QUIT(msg))

/**
 * PROTOCOL_OVERHEAD - Get the protocol overhead in bytes
 * (magic number + delimiters + null terminator)
 */
#define PROTOCOL_OVERHEAD (sizeof(csend_uint32_t) + 3)

#endif /* PROTOCOL_H */