#include "protocol.h"
#include "logging.h"
#include <stdio.h>
#include <string.h>
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
int format_message(char *buffer, int buffer_size, const char *msg_type,
                   const char *sender, const char *local_ip_str, const char *content)
{
    char sender_with_ip[BUFFER_SIZE];
    const char *ip_to_use;
    csend_uint32_t magic_host_order = MSG_MAGIC_NUMBER;
    csend_uint32_t magic_net_order;
    int sender_len;
    int text_part_len;
    int total_len;
    if (buffer == NULL || buffer_size < (int)(sizeof(csend_uint32_t) + 1)) {
        log_debug("Error: format_message buffer too small (%d bytes, need at least %d) or NULL.",
                  buffer_size, (int)(sizeof(csend_uint32_t) + 1));
        return 0;
    }
    magic_net_order = htonl(magic_host_order);
    memcpy(buffer, &magic_net_order, sizeof(csend_uint32_t));
    if (local_ip_str != NULL && local_ip_str[0] != '\0') {
        ip_to_use = local_ip_str;
    } else {
        log_debug("Warning: format_message received NULL or empty local_ip_str. Using 'unknown'.");
        ip_to_use = "unknown";
    }
    sender_len = snprintf(sender_with_ip, sizeof(sender_with_ip), "%s@%s", sender ? sender : "anon", ip_to_use);
    if (sender_len < 0 || sender_len >= (int)sizeof(sender_with_ip)) {
        log_debug("Error: format_message failed formatting sender@ip or buffer too small.");
        return 0;
    }
    int remaining_buffer_size = buffer_size - sizeof(csend_uint32_t);
    char *text_buffer_start = buffer + sizeof(csend_uint32_t);
    text_part_len = snprintf(text_buffer_start, remaining_buffer_size, "%s|%s|%s",
                             msg_type ? msg_type : "UNKNOWN",
                             sender_with_ip,
                             content ? content : "");
    if (text_part_len >= remaining_buffer_size) {
        log_debug("Warning: format_message text part truncated (buffer size %d, needed %d for text + NUL).",
                  remaining_buffer_size, text_part_len + 1);
        return 0;
    }
    if (text_part_len < 0) {
        log_debug("Error: format_message failed formatting final text part (snprintf encoding error).");
        return 0;
    }
    total_len = sizeof(csend_uint32_t) + text_part_len + 1;
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
    char temp_text_part[BUFFER_SIZE];
    char sender_with_ip[BUFFER_SIZE];
    csend_uint32_t received_magic_net_order;
    csend_uint32_t received_magic_host_order;
    const char *text_part_start;
    int text_part_len;
    if (sender_ip) sender_ip[0] = '\0';
    if (sender_username) sender_username[0] = '\0';
    if (msg_type) msg_type[0] = '\0';
    if (content) content[0] = '\0';
    if (buffer == NULL || buffer_len < (int)sizeof(csend_uint32_t)) {
        log_debug("Parse error: Buffer NULL or too short for magic number (%d bytes, need %d).",
                  buffer_len, (int)sizeof(csend_uint32_t));
        return -1;
    }
    memcpy(&received_magic_net_order, buffer, sizeof(csend_uint32_t));
    received_magic_host_order = ntohl(received_magic_net_order);
    if (received_magic_host_order != MSG_MAGIC_NUMBER) {
        log_debug("Parse error: Invalid magic number. Expected %08lX, got %08lX.",
                  (unsigned long)MSG_MAGIC_NUMBER, (unsigned long)received_magic_host_order);
        return -1;
    }
    text_part_start = buffer + sizeof(csend_uint32_t);
    text_part_len = buffer_len - sizeof(csend_uint32_t);
    if (text_part_len < 0) text_part_len = 0;
    if (text_part_len >= (int)sizeof(temp_text_part)) {
        log_debug("Parse warning: Text part length (%d) exceeds temp buffer (%d). Truncating.",
                  text_part_len, (int)sizeof(temp_text_part) - 1);
        text_part_len = sizeof(temp_text_part) - 1;
    }
    strncpy(temp_text_part, text_part_start, text_part_len);
    temp_text_part[text_part_len] = '\0';
    token = strtok_r(temp_text_part, "|", &rest);
    if (token == NULL) {
        log_debug("Parse error: Could not find message type token.");
        return -1;
    }
    if (msg_type) {
        strncpy(msg_type, token, 31);
        msg_type[31] = '\0';
    }
    token = strtok_r(NULL, "|", &rest);
    if (token == NULL) {
        log_debug("Parse error: Could not find sender@ip token.");
        return -1;
    }
    strncpy(sender_with_ip, token, sizeof(sender_with_ip) - 1);
    sender_with_ip[sizeof(sender_with_ip) - 1] = '\0';
    char *at_sign = strchr(sender_with_ip, '@');
    if (at_sign != NULL) {
        int username_len = at_sign - sender_with_ip;
        if (sender_username) {
            if (username_len > 31) username_len = 31;
            strncpy(sender_username, sender_with_ip, username_len);
            sender_username[username_len] = '\0';
        }
        if (sender_ip) {
            strncpy(sender_ip, at_sign + 1, INET_ADDRSTRLEN - 1);
            sender_ip[INET_ADDRSTRLEN - 1] = '\0';
        }
    } else {
        log_debug("Parse warning: '@' not found in sender token '%s'. Treating as username.", sender_with_ip);
        if (sender_username) {
            strncpy(sender_username, sender_with_ip, 31);
            sender_username[31] = '\0';
        }
        if (sender_ip) {
            strcpy(sender_ip, "unknown");
        }
    }
    token = strtok_r(NULL, "", &rest);
    if (token == NULL) {
        if (content) content[0] = '\0';
    } else {
        if (content) {
            strncpy(content, token, BUFFER_SIZE - 1);
            content[BUFFER_SIZE - 1] = '\0';
        }
    }
    return 0;
}
