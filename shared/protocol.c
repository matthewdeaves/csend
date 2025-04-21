// FILE: ./shared/protocol.c
// Include the header file for this module, which declares the functions defined here
// and defines message type constants (like MSG_TEXT) and the magic number.
#include "protocol.h"

// Include the utils header for the log_message() function, used for logging
// warnings and errors within the protocol formatting and parsing logic.
#include "logging.h"

// Standard C library for input/output functions, specifically snprintf for safe string formatting.
#include <stdio.h>
// Standard C library for string manipulation functions:
// strcpy: For copying strings (used for fallback IP).
// strncpy: For safely copying strings with length limits.
// strtok_r: For tokenizing strings (re-entrant version, safe for threads).
// strchr: For finding characters within strings (used to find '@').
// memcpy: For copying the magic number bytes.
#include <string.h>

// Platform-specific includes for byte order conversion
#ifdef __MACOS__
    // MacTCP.h or similar might define these, or we do it manually
    // Manual Big Endian conversion (Network Byte Order is Big Endian)
    #define my_htonl(l) (l) // Assume Classic Mac is already Big Endian
    #define my_ntohl(l) (l) // Assume Classic Mac is already Big Endian
    typedef unsigned long uint32_t; // Define if not available
#else // POSIX
    #include <arpa/inet.h> // For htonl, ntohl
    #include <stdint.h>    // For uint32_t
    #define my_htonl(l) htonl(l)
    #define my_ntohl(l) ntohl(l)
#endif


// --- Protocol Definition ---
// Format: MAGIC(4 bytes)|TYPE|SENDER@IP|CONTENT
// MAGIC: Network Byte Order (Big Endian) representation of MSG_MAGIC_NUMBER
// Example Text Part: "TEXT|alice@192.168.1.10|Hello Bob!"

/**
 * @brief Formats a message including the magic number and protocol string.
 * @details Writes the 4-byte magic number (network byte order) followed by "TYPE|SENDER@IP|CONTENT".
 * @param buffer Output buffer.
 * @param buffer_size Total size of the output buffer.
 * @param msg_type Message type string.
 * @param sender Sender username string.
 * @param local_ip_str Sender's local IP string.
 * @param content Message content string.
 * @return Total bytes written (including magic number and null terminator) on success.
 * @return 0 if buffer_size is too small or on encoding error.
 */
int format_message(char *buffer, int buffer_size, const char *msg_type,
                   const char *sender, const char *local_ip_str, const char *content) {
    char sender_with_ip[BUFFER_SIZE]; // Consider a smaller, fixed size? e.g., 32 + 1 + 16 = 49
    const char *ip_to_use;
    uint32_t magic_net_order;
    int sender_len;
    int text_part_len;
    int total_len;

    // --- Basic Validation ---
    if (buffer == NULL || buffer_size < (int)(sizeof(uint32_t) + 1)) { // Need space for magic + null term at minimum
        log_message("Error: format_message buffer too small (%d bytes) or NULL.", buffer_size);
        return 0;
    }

    // --- Prepare Magic Number ---
    magic_net_order = my_htonl(MSG_MAGIC_NUMBER);
    memcpy(buffer, &magic_net_order, sizeof(uint32_t));

    // --- Prepare Sender@IP ---
    if (local_ip_str != NULL && local_ip_str[0] != '\0') {
        ip_to_use = local_ip_str;
    } else {
        log_message("Warning: format_message received NULL or empty local_ip_str. Using 'unknown'.");
        ip_to_use = "unknown";
    }

    sender_len = snprintf(sender_with_ip, sizeof(sender_with_ip), "%s@%s", sender ? sender : "anon", ip_to_use);
    if (sender_len < 0 || sender_len >= (int)sizeof(sender_with_ip)) {
        log_message("Error: format_message failed formatting sender@ip.");
        return 0;
    }

    // --- Format Text Part (after magic number) ---
    // Calculate remaining buffer space for the text part + null terminator
    int remaining_buffer_size = buffer_size - sizeof(uint32_t);
    char *text_buffer_start = buffer + sizeof(uint32_t);

    text_part_len = snprintf(text_buffer_start, remaining_buffer_size, "%s|%s|%s",
                             msg_type ? msg_type : "UNKNOWN",
                             sender_with_ip,
                             content ? content : "");

    // Check for truncation or encoding errors in the text part
    if (text_part_len >= remaining_buffer_size) {
        log_message("Warning: format_message text part truncated (buffer size %d, needed %d).", remaining_buffer_size, text_part_len + 1);
        return 0; // Indicate failure (truncation)
    }
    if (text_part_len < 0) {
        log_message("Error: format_message failed formatting final text part (encoding error).");
        return 0; // Indicate failure (encoding error)
    }

    // --- Calculate Total Length ---
    // Total length includes magic number bytes + text part bytes + null terminator byte
    total_len = sizeof(uint32_t) + text_part_len + 1;

    // Final sanity check against original buffer size
    if (total_len > buffer_size) {
         // This case should theoretically be caught by the snprintf check above, but belt-and-suspenders
         log_message("Error: format_message internal logic error - total_len > buffer_size.");
         return 0;
    }

    return total_len; // Return total bytes written (including null term)
}


/**
 * @brief Parses an incoming message string after verifying the magic number.
 * @details Checks for magic number, then parses "TYPE|SENDER@IP|CONTENT" from the rest.
 * @param buffer The constant input buffer containing raw network data.
 * @param buffer_len The actual number of bytes received in the buffer.
 * @param sender_ip Output buffer for sender IP.
 * @param sender_username Output buffer for sender username.
 * @param msg_type Output buffer for message type.
 * @param content Output buffer for message content.
 * @return 0 on success.
 * @return -1 if magic number is missing/wrong, buffer_len too short, or parsing fails.
 */
int parse_message(const char *buffer, int buffer_len, char *sender_ip, char *sender_username,
                  char *msg_type, char *content) {
    char *token;
    char *rest;
    // Temp buffer for the TEXT part only (after magic number)
    char temp_text_part[BUFFER_SIZE];
    char sender_with_ip[BUFFER_SIZE]; // Consider smaller size?
    uint32_t received_magic;
    const char *text_part_start;
    int text_part_len;

    // --- Clear output buffers initially ---
    sender_ip[0] = '\0';
    sender_username[0] = '\0';
    msg_type[0] = '\0';
    content[0] = '\0';

    // --- Step 1: Check Magic Number ---
    if (buffer == NULL || buffer_len < (int)sizeof(uint32_t)) {
        // Not enough data even for the magic number
        // log_message("Parse error: Received packet too short for magic number (%d bytes).", buffer_len); // Can be noisy
        return -1;
    }

    // Read the magic number from the start of the buffer
    memcpy(&received_magic, buffer, sizeof(uint32_t));
    received_magic = my_ntohl(received_magic); // Convert from network to host order

    if (received_magic != MSG_MAGIC_NUMBER) {
        // Magic number mismatch - likely unrelated packet
        // log_message("Parse error: Invalid magic number (Expected 0x%lX, Got 0x%lX). Discarding.", MSG_MAGIC_NUMBER, received_magic); // Can be noisy
        return -1;
    }

    // --- Step 2: Prepare for Text Part Parsing ---
    text_part_start = buffer + sizeof(uint32_t);
    // Calculate the length of the text part (including potential null terminator if present)
    text_part_len = buffer_len - sizeof(uint32_t);

    // Create a mutable, null-terminated copy of the text part for strtok_r
    if (text_part_len >= BUFFER_SIZE) {
        log_message("Parse warning: Text part length (%d) exceeds temp buffer (%d). Truncating.", text_part_len, BUFFER_SIZE - 1);
        text_part_len = BUFFER_SIZE - 1; // Prevent overflow
    }
    strncpy(temp_text_part, text_part_start, text_part_len);
    temp_text_part[text_part_len] = '\0'; // Ensure null termination for strtok_r

    // --- Step 3: Parse Message Type (from text part) ---
    token = strtok_r(temp_text_part, "|", &rest);
    if (token == NULL) {
        log_message("Parse error: Could not find message type token after magic number.");
        return -1;
    }
    strncpy(msg_type, token, 31);
    msg_type[31] = '\0';

    // --- Step 4: Parse Sender@IP (from text part) ---
    token = strtok_r(NULL, "|", &rest);
    if (token == NULL) {
        log_message("Parse error: Could not find sender@ip token after magic number.");
        return -1;
    }
    strncpy(sender_with_ip, token, sizeof(sender_with_ip) - 1);
    sender_with_ip[sizeof(sender_with_ip) - 1] = '\0';

    // --- Step 5: Split Sender@IP ---
    char *at_sign = strchr(sender_with_ip, '@');
    if (at_sign != NULL) {
        int username_len = at_sign - sender_with_ip;
        if (username_len > 31) username_len = 31;
        strncpy(sender_username, sender_with_ip, username_len);
        sender_username[username_len] = '\0';

        strncpy(sender_ip, at_sign + 1, INET_ADDRSTRLEN - 1);
        sender_ip[INET_ADDRSTRLEN - 1] = '\0';
    } else {
        log_message("Parse warning: '@' not found in sender token '%s'. Treating as username.", sender_with_ip);
        strncpy(sender_username, sender_with_ip, 31);
        sender_username[31] = '\0';
        strcpy(sender_ip, "unknown");
    }

    // --- Step 6: Parse Content (from text part) ---
    token = strtok_r(NULL, "", &rest); // Get the remainder
    if (token == NULL) {
        content[0] = '\0'; // No content part
    } else {
        // Use BUFFER_SIZE for the content buffer limit
        strncpy(content, token, BUFFER_SIZE - 1);
        content[BUFFER_SIZE - 1] = '\0';
    }

    return 0; // Success
}