#include "protocol.h"
#include "logging.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>  /* Added for malloc/free */

#ifdef __MACOS__
#include <MacTCP.h>
static inline csend_uint32_t csend_plat_htonl(csend_uint32_t hostlong)
{
    return hostlong;
}
static inline csend_uint32_t csend_plat_ntohl(csend_uint32_t netlong)
{
    return netlong;
}
#define htonl(x) csend_plat_htonl(x)
#define ntohl(x) csend_plat_ntohl(x)
#else
#include <arpa/inet.h>
#endif

/* Internal constants for protocol limits */
#define MAX_MSG_TYPE_LEN 31
#define MAX_USERNAME_LEN 31
#define MIN_PROTOCOL_SIZE ((int)(sizeof(csend_uint32_t) + 3)) /* magic + "||" - cast to int */

/* Helper function to safely copy strings with guaranteed null termination */
static void safe_strncpy(char *dest, const char *src, size_t dest_size)
{
    if (dest == NULL || dest_size == 0) return;

    if (src == NULL) {
        dest[0] = '\0';
        return;
    }

    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

int format_message(char *buffer, int buffer_size, const char *msg_type,
                   const char *sender, const char *local_ip_str, const char *content)
{
    char sender_with_ip[BUFFER_SIZE];
    const char *ip_to_use;
    const char *safe_sender;
    const char *safe_msg_type;
    const char *safe_content;
    csend_uint32_t magic_host_order = MSG_MAGIC_NUMBER;
    csend_uint32_t magic_net_order;
    int sender_len;
    int text_part_len;
    int total_len;

    /* Validate minimum buffer requirements */
    if (buffer == NULL || buffer_size < MIN_PROTOCOL_SIZE) {
        log_debug("Error: format_message buffer too small (%d bytes, need at least %d) or NULL.",
                  buffer_size, MIN_PROTOCOL_SIZE);
        return 0;
    }

    /* Sanitize inputs to prevent NULL pointer issues */
    safe_sender = (sender != NULL) ? sender : "anon";
    safe_msg_type = (msg_type != NULL) ? msg_type : "UNKNOWN";
    safe_content = (content != NULL) ? content : "";

    /* Validate IP string */
    if (local_ip_str != NULL && local_ip_str[0] != '\0') {
        ip_to_use = local_ip_str;
    } else {
        log_debug("Warning: format_message received NULL or empty local_ip_str. Using 'unknown'.");
        ip_to_use = "unknown";
    }

    /* Format sender@ip with bounds checking */
    sender_len = snprintf(sender_with_ip, sizeof(sender_with_ip), "%s@%s", safe_sender, ip_to_use);
    if (sender_len < 0 || sender_len >= (int)sizeof(sender_with_ip)) {
        log_debug("Error: format_message failed formatting sender@ip or buffer too small.");
        return 0;
    }

    /* Write magic number */
    magic_net_order = htonl(magic_host_order);
    memcpy(buffer, &magic_net_order, sizeof(csend_uint32_t));

    /* Calculate remaining space and format text part */
    int remaining_buffer_size = buffer_size - (int)sizeof(csend_uint32_t);
    char *text_buffer_start = buffer + sizeof(csend_uint32_t);

    text_part_len = snprintf(text_buffer_start, remaining_buffer_size, "%s|%s|%s",
                             safe_msg_type, sender_with_ip, safe_content);

    /* Check for truncation or error */
    if (text_part_len < 0) {
        log_debug("Error: format_message failed formatting final text part (snprintf encoding error).");
        return 0;
    }

    if (text_part_len >= remaining_buffer_size) {
        log_debug("Warning: format_message text part truncated (buffer size %d, needed %d for text + NUL).",
                  remaining_buffer_size, text_part_len + 1);
        return 0;
    }

    /* Calculate total message length including null terminator */
    total_len = (int)sizeof(csend_uint32_t) + text_part_len + 1;

    /* Sanity check (should never happen with correct logic above) */
    if (total_len > buffer_size) {
        log_debug("Error: format_message internal logic error - calculated total_len %d > buffer_size %d.",
                  total_len, buffer_size);
        return 0;
    }

    return total_len;
}

int parse_message(const char *buffer, int buffer_len, char *sender_ip, char *sender_username,
                  char *msg_type, char *content)
{
    char *token;
    char *rest;
    char *temp_buffer = NULL;
    char sender_with_ip[BUFFER_SIZE];
    csend_uint32_t received_magic_net_order;
    csend_uint32_t received_magic_host_order;
    const char *text_part_start;
    int text_part_len;
    int result = -1;

    /* Initialize output parameters */
    if (sender_ip) sender_ip[0] = '\0';
    if (sender_username) sender_username[0] = '\0';
    if (msg_type) msg_type[0] = '\0';
    if (content) content[0] = '\0';

    /* Validate minimum buffer requirements */
    if (buffer == NULL || buffer_len < MIN_PROTOCOL_SIZE) {
        log_debug("Parse error: Buffer NULL or too short (%d bytes, need at least %d).",
                  buffer_len, MIN_PROTOCOL_SIZE);
        return -1;
    }

    /* Verify magic number */
    memcpy(&received_magic_net_order, buffer, sizeof(csend_uint32_t));
    received_magic_host_order = ntohl(received_magic_net_order);
    if (received_magic_host_order != MSG_MAGIC_NUMBER) {
        log_debug("Parse error: Invalid magic number. Expected %08lX, got %08lX.",
                  (unsigned long)MSG_MAGIC_NUMBER, (unsigned long)received_magic_host_order);
        return -1;
    }

    /* Calculate text part boundaries */
    text_part_start = buffer + sizeof(csend_uint32_t);
    text_part_len = buffer_len - (int)sizeof(csend_uint32_t);

    /* Ensure we have at least some text to parse */
    if (text_part_len <= 0) {
        log_debug("Parse error: No text part after magic number.");
        return -1;
    }

    /* Allocate temporary buffer for safe string manipulation */
    /* Add 1 for null terminator */
    temp_buffer = (char *)malloc(text_part_len + 1);
    if (temp_buffer == NULL) {
        log_debug("Parse error: Failed to allocate temporary buffer (%d bytes).", text_part_len + 1);
        return -1;
    }

    /* Copy text part with guaranteed null termination */
    memcpy(temp_buffer, text_part_start, text_part_len);
    temp_buffer[text_part_len] = '\0';

    /* Parse message type */
    token = strtok_r(temp_buffer, "|", &rest);
    if (token == NULL) {
        log_debug("Parse error: Could not find message type token.");
        goto cleanup;
    }
    if (msg_type) {
        safe_strncpy(msg_type, token, MAX_MSG_TYPE_LEN + 1);
    }

    /* Parse sender@ip */
    token = strtok_r(NULL, "|", &rest);
    if (token == NULL) {
        log_debug("Parse error: Could not find sender@ip token.");
        goto cleanup;
    }
    safe_strncpy(sender_with_ip, token, sizeof(sender_with_ip));

    /* Split sender@ip into components */
    char *at_sign = strchr(sender_with_ip, '@');
    if (at_sign != NULL) {
        /* Calculate username length safely */
        int username_len = (int)(at_sign - sender_with_ip);

        /* Extract username */
        if (sender_username) {
            if (username_len > MAX_USERNAME_LEN) {
                username_len = MAX_USERNAME_LEN;
            }
            memcpy(sender_username, sender_with_ip, username_len);
            sender_username[username_len] = '\0';
        }

        /* Extract IP address */
        if (sender_ip) {
            safe_strncpy(sender_ip, at_sign + 1, INET_ADDRSTRLEN);
        }
    } else {
        log_debug("Parse warning: '@' not found in sender token '%s'. Treating as username.", sender_with_ip);
        if (sender_username) {
            safe_strncpy(sender_username, sender_with_ip, MAX_USERNAME_LEN + 1);
        }
        if (sender_ip) {
            strcpy(sender_ip, "unknown");
        }
    }

    /* Parse content (everything after second |) */
    token = strtok_r(NULL, "", &rest);
    if (token == NULL) {
        /* No content is valid - just ensure it's empty */
        if (content) content[0] = '\0';
    } else {
        if (content) {
            /* Calculate safe copy length */
            int content_len = (int)strlen(token);
            if (content_len >= BUFFER_SIZE) {
                content_len = BUFFER_SIZE - 1;
                log_debug("Parse warning: Content truncated from %d to %d bytes.",
                          (int)strlen(token), content_len);
            }
            memcpy(content, token, content_len);
            content[content_len] = '\0';
        }
    }

    /* Success */
    result = 0;

cleanup:
    if (temp_buffer) {
        free(temp_buffer);
    }
    return result;
}