// Include the header file for this module, which declares the functions defined here
// and defines message type constants (like MSG_TEXT). It also includes peer.h,
// providing necessary constants like BUFFER_SIZE and INET_ADDRSTRLEN.
#include "protocol.h"

// Include the network header for the get_local_ip() function, needed to embed
// the sender's IP address in outgoing messages.
#include "network.h"

// Include the utils header for the log_message() function, used for logging
// warnings and errors within the protocol formatting and parsing logic.
#include "utils.h"

// Standard C library for input/output functions, specifically snprintf for safe string formatting.
#include <stdio.h>
// Standard C library for string manipulation functions:
// strcpy: For copying strings (used for fallback IP).
// strncpy: For safely copying strings with length limits.
// strtok_r: For tokenizing strings (re-entrant version, safe for threads).
// strchr: For finding characters within strings (used to find '@').
#include <string.h>

// --- Protocol Definition ---
// The application uses a simple text-based protocol with fields separated by pipe characters ('|').
// Format: TYPE|SENDER@IP|CONTENT
// Example: "TEXT|alice@192.168.1.10|Hello Bob!"
// Example: "DISCOVERY|bob@192.168.1.11|" (Content can be empty)

/**
 * @brief Formats a message according to the application's protocol string format.
 * @details Constructs a string in the format "TYPE|SENDER@IP|CONTENT".
 *          It retrieves the local machine's IP address using `get_local_ip`
 *          to include in the SENDER@IP part.
 * @param buffer A pointer to the character buffer where the formatted message string will be written.
 *               The caller must ensure this buffer is large enough.
 * @param buffer_size The total size (in bytes) of the `buffer`. Used by `snprintf` to prevent overflows.
 * @param msg_type A string representing the message type (e.g., "TEXT", "DISCOVERY", "QUIT").
 * @param sender The username of the peer sending the message.
 * @param content The main payload/content of the message. Can be an empty string.
 * @return 0 on success.
 * @return -1 if the formatted message (including null terminator) would exceed `buffer_size`.
 *         In this case, the content of `buffer` might be truncated but will be null-terminated
 *         if `buffer_size` > 0.
 */
int format_message(char *buffer, int buffer_size, const char *msg_type,
                  const char *sender, const char *content) {
    // Buffer to temporarily hold the "sender@ip" part of the message.
    // Size should be adequate for username + '@' + IP address + null terminator.
    // Using BUFFER_SIZE might be overkill but is safe. A smaller, calculated size could be used.
    char sender_with_ip[BUFFER_SIZE]; // Consider a smaller size like 32 + 1 + INET_ADDRSTRLEN ?
    // Buffer to hold the local IP address string.
    char local_ip[INET_ADDRSTRLEN];

    // Attempt to retrieve the local IP address.
    if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
        // If getting the local IP fails, use "unknown" as a fallback.
        // This allows message formatting to proceed but indicates an issue.
        log_message("Warning: format_message failed to get local IP. Using 'unknown'.");
        strcpy(local_ip, "unknown");
    }

    // Format the "sender@ip" part using snprintf for safety.
    // Writes "username@ip_address" into the sender_with_ip buffer.
    // Returns the number of characters that *would* have been written (excluding null term).
    int sender_len = snprintf(sender_with_ip, sizeof(sender_with_ip), "%s@%s", sender, local_ip);
    // Basic check if sender formatting failed or truncated (though unlikely with BUFFER_SIZE).
     if (sender_len < 0 || sender_len >= (int)sizeof(sender_with_ip)) {
         log_message("Error: format_message failed formatting sender@ip (buffer too small or encoding error).");
         return -1;
     }


    // Format the complete message string: "TYPE|SENDER@IP|CONTENT".
    // Use snprintf to write the final formatted string into the output `buffer`.
    // `snprintf` prevents buffer overflows by writing at most `buffer_size - 1` characters
    // and always appending a null terminator if `buffer_size` > 0.
    int result = snprintf(buffer, buffer_size, "%s|%s|%s",
                         msg_type, sender_with_ip, content);

    // Check if snprintf truncated the output.
    // `result` holds the number of characters (excluding null terminator) that *would*
    // have been written if the buffer was large enough.
    // If `result` is greater than or equal to `buffer_size`, it means truncation occurred.
    if (result >= buffer_size) {
        log_message("Warning: format_message output truncated (buffer size %d, needed %d).", buffer_size, result + 1);
        return -1; // Indicate failure due to insufficient buffer space.
    }
     if (result < 0) {
         log_message("Error: format_message failed formatting final message (encoding error).");
         return -1; // Indicate failure due to encoding error
     }


    // Formatting was successful and fit within the buffer.
    return 0;
}

/**
 * @brief Parses an incoming message string according to the application protocol.
 * @details Deconstructs a message string (expected format: "TYPE|SENDER@IP|CONTENT")
 *          into its constituent parts: message type, sender username, sender IP, and content.
 *          It uses `strtok_r` for tokenization, which modifies a temporary copy of the input buffer.
 * @param buffer The constant input character buffer containing the raw message string received from the network.
 * @param sender_ip Output buffer (provided by caller) to store the extracted sender IP address string.
 *                  Should be at least `INET_ADDRSTRLEN` bytes.
 * @param sender_username Output buffer (provided by caller) to store the extracted sender username string.
 *                        Should be at least 32 bytes.
 * @param msg_type Output buffer (provided by caller) to store the extracted message type string.
 *                 Should be at least 32 bytes.
 * @param content Output buffer (provided by caller) to store the extracted message content string.
 *                Should be at least `BUFFER_SIZE` bytes (or large enough for expected content).
 * @return 0 on successful parsing of all expected parts.
 * @return -1 on parsing failure, typically indicating the input `buffer` does not conform
 *         to the expected "TYPE|SENDER@IP|CONTENT" format (e.g., missing '|' delimiters).
 * @note Assumes output buffers are sufficiently large. Uses `strncpy` for safety when copying
 *       extracted parts, but relies on the caller providing adequate buffer sizes.
 * @warning Does not perform deep validation (e.g., checking if IP is valid format, username chars).
 */
int parse_message(const char *buffer, char *sender_ip, char *sender_username, char *msg_type, char *content) {
    // Pointer used by strtok_r to store the next token.
    char *token;
    // Pointer used by strtok_r to keep track of the remaining string being tokenized.
    char *rest;
    // Temporary buffer to hold a mutable copy of the input buffer, as strtok_r modifies the string it parses.
    char temp[BUFFER_SIZE];
    // Temporary buffer to hold the extracted "SENDER@IP" part before splitting it further.
    char sender_with_ip[BUFFER_SIZE]; // Again, consider smaller size?

    // --- Pre-parsing setup ---
    // Clear output buffers initially (optional, but good practice)
    sender_ip[0] = '\0';
    sender_username[0] = '\0';
    msg_type[0] = '\0';
    content[0] = '\0';


    // Create a mutable copy of the input buffer.
    // Use strncpy to avoid overflow if the input `buffer` is unexpectedly large (though unlikely if read into BUFFER_SIZE).
    strncpy(temp, buffer, BUFFER_SIZE - 1);
    // Ensure the temporary copy is null-terminated.
    temp[BUFFER_SIZE - 1] = '\0';

    // --- Parse Message Type (Part 1) ---
    // Get the first token using '|' as the delimiter.
    // `strtok_r` is thread-safe compared to `strtok`.
    // `temp`: The string to tokenize (will be modified).
    // "|": The delimiter characters.
    // `&rest`: Pointer to save the context for subsequent calls.
    token = strtok_r(temp, "|", &rest);
    if (token == NULL) {
        // If the first token is NULL, the string is empty or doesn't contain '|'. Invalid format.
        log_message("Parse error: Could not find message type token.");
        return -1;
    }
    // Copy the extracted type token into the output buffer, ensuring safety and null termination.
    // Assuming msg_type buffer is 32 bytes.
    strncpy(msg_type, token, 31);
    msg_type[31] = '\0';

    // --- Parse Sender@IP (Part 2) ---
    // Get the next token (the part between the first and second '|').
    // Pass NULL as the first argument to continue tokenizing the same string (`rest` maintains context).
    token = strtok_r(NULL, "|", &rest);
    if (token == NULL) {
        // If the second token is NULL, the required SENDER@IP part is missing. Invalid format.
        log_message("Parse error: Could not find sender@ip token.");
        return -1;
    }
    // Copy the extracted "sender@ip" token into a temporary buffer for further processing.
    // Using BUFFER_SIZE is safe but potentially wasteful.
    strncpy(sender_with_ip, token, sizeof(sender_with_ip) - 1);
    sender_with_ip[sizeof(sender_with_ip) - 1] = '\0'; // Ensure null termination

    // --- Split Sender@IP into Username and IP ---
    // Find the '@' character within the "sender@ip" string.
    char *at_sign = strchr(sender_with_ip, '@');
    if (at_sign != NULL) {
        // '@' sign found.

        // Calculate the length of the username part (characters before '@').
        int username_len = at_sign - sender_with_ip;
        // Copy the username part into the output buffer.
        // Ensure the length doesn't exceed the buffer size (31 chars + null term).
        if (username_len > 31) username_len = 31; // Cap length if needed
        strncpy(sender_username, sender_with_ip, username_len);
        sender_username[username_len] = '\0'; // Manually null-terminate.

        // Copy the IP address part (characters after '@') into the output buffer.
        // `at_sign + 1` points to the character immediately after '@'.
        // Ensure safety with strncpy and null termination.
        strncpy(sender_ip, at_sign + 1, INET_ADDRSTRLEN - 1);
        sender_ip[INET_ADDRSTRLEN - 1] = '\0';

    } else {
        // '@' sign not found in the second token. This might be an error, or maybe
        // the protocol allows just a username or IP sometimes?
        // Current behavior: Assume the whole token is the username and IP is unknown.
        log_message("Parse warning: '@' not found in sender token '%s'. Treating as username.", sender_with_ip);
        strncpy(sender_username, sender_with_ip, 31); // Copy token as username
        sender_username[31] = '\0';
        strcpy(sender_ip, "unknown"); // Set IP as unknown
        // Depending on requirements, might want to return -1 here if '@' is mandatory.
    }

    // --- Parse Content (Part 3) ---
    // Get the rest of the string after the second '|' as the content.
    // Using "" as delimiters tells strtok_r to return the remainder of the string.
    // Note: If the original string was "TYPE|SENDER@IP|", `rest` points to an empty string,
    // and `token` will be that empty string. If it was "TYPE|SENDER@IP", `rest` is NULL,
    // and `token` will be NULL.
    token = strtok_r(NULL, "", &rest); // Get the remainder
    if (token == NULL) {
        // No content part found (e.g., message ended after SENDER@IP).
        // Set content to an empty string.
        content[0] = '\0';
    } else {
        // Copy the content token into the output buffer.
        strncpy(content, token, BUFFER_SIZE - 1);
        content[BUFFER_SIZE - 1] = '\0'; // Ensure null termination.
    }

    // All parts parsed successfully (or handled gracefully).
    return 0;
}