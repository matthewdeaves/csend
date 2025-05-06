#include "protocol.h"
#include "logging.h"
#include <stdio.h>
#include <string.h>
#ifdef __MACOS__
#define my_htonl(l) (l)
#define my_ntohl(l) (l)
typedef unsigned long uint32_t;
#else
#include <arpa/inet.h>
#include <stdint.h>
#define my_htonl(l) htonl(l)
#define my_ntohl(l) ntohl(l)
#endif
int format_message(char *buffer, int buffer_size, const char *msg_type,
                   const char *sender, const char *local_ip_str, const char *content)
{
    char sender_with_ip[BUFFER_SIZE];
    const char *ip_to_use;
    uint32_t magic_net_order;
    int sender_len;
    int text_part_len;
    int total_len;
    if (buffer == NULL || buffer_size < (int)(sizeof(uint32_t) + 1)) {
        log_message("Error: format_message buffer too small (%d bytes) or NULL.", buffer_size);
        return 0;
    }
    magic_net_order = my_htonl(MSG_MAGIC_NUMBER);
    memcpy(buffer, &magic_net_order, sizeof(uint32_t));
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
    int remaining_buffer_size = buffer_size - sizeof(uint32_t);
    char *text_buffer_start = buffer + sizeof(uint32_t);
    text_part_len = snprintf(text_buffer_start, remaining_buffer_size, "%s|%s|%s",
                             msg_type ? msg_type : "UNKNOWN",
                             sender_with_ip,
                             content ? content : "");
    if (text_part_len >= remaining_buffer_size) {
        log_message("Warning: format_message text part truncated (buffer size %d, needed %d).", remaining_buffer_size, text_part_len + 1);
        return 0;
    }
    if (text_part_len < 0) {
        log_message("Error: format_message failed formatting final text part (encoding error).");
        return 0;
    }
    total_len = sizeof(uint32_t) + text_part_len + 1;
    if (total_len > buffer_size) {
        log_message("Error: format_message internal logic error - total_len > buffer_size.");
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
    uint32_t received_magic;
    const char *text_part_start;
    int text_part_len;
    sender_ip[0] = '\0';
    sender_username[0] = '\0';
    msg_type[0] = '\0';
    content[0] = '\0';
    if (buffer == NULL || buffer_len < (int)sizeof(uint32_t)) {
        return -1;
    }
    memcpy(&received_magic, buffer, sizeof(uint32_t));
    received_magic = my_ntohl(received_magic);
    if (received_magic != MSG_MAGIC_NUMBER) {
        return -1;
    }
    text_part_start = buffer + sizeof(uint32_t);
    text_part_len = buffer_len - sizeof(uint32_t);
    if (text_part_len >= BUFFER_SIZE) {
        log_message("Parse warning: Text part length (%d) exceeds temp buffer (%d). Truncating.", text_part_len, BUFFER_SIZE - 1);
        text_part_len = BUFFER_SIZE - 1;
    }
    strncpy(temp_text_part, text_part_start, text_part_len);
    temp_text_part[text_part_len] = '\0';
    token = strtok_r(temp_text_part, "|", &rest);
    if (token == NULL) {
        log_message("Parse error: Could not find message type token after magic number.");
        return -1;
    }
    strncpy(msg_type, token, 31);
    msg_type[31] = '\0';
    token = strtok_r(NULL, "|", &rest);
    if (token == NULL) {
        log_message("Parse error: Could not find sender@ip token after magic number.");
        return -1;
    }
    strncpy(sender_with_ip, token, sizeof(sender_with_ip) - 1);
    sender_with_ip[sizeof(sender_with_ip) - 1] = '\0';
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
    token = strtok_r(NULL, "", &rest);
    if (token == NULL) {
        content[0] = '\0';
    } else {
        strncpy(content, token, BUFFER_SIZE - 1);
        content[BUFFER_SIZE - 1] = '\0';
    }
    return 0;
}
