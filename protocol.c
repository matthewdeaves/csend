#include "peer.h"

// Message format: TYPE|SENDER|CONTENT
// Example: TEXT|username@192.168.1.5|Hello, world!

int format_message(char *buffer, int buffer_size, const char *msg_type, 
                  const char *sender, const char *content) {
    char sender_with_ip[BUFFER_SIZE];
    char local_ip[INET_ADDRSTRLEN];
    
    // Get local IP address
    if (get_local_ip(local_ip, INET_ADDRSTRLEN) < 0) {
        strcpy(local_ip, "unknown");
    }
    
    // Format sender with IP
    snprintf(sender_with_ip, BUFFER_SIZE, "%s@%s", sender, local_ip);
    
    // Format complete message
    int result = snprintf(buffer, buffer_size, "%s|%s|%s", 
                         msg_type, sender_with_ip, content);
    
    if (result >= buffer_size) {
        return -1; // Buffer too small
    }
    
    return 0;
}

/*
 * This function breaks down a message string into its component parts according to the
 * messaging protocol. It extracts the message type, sender information
 * (username and IP address), and the message content.
 * It performs basic format validation but does not verify the
 * authenticity or integrity of the message. It assumes all output buffers
 * are large enough to hold their respective components.
*/
int parse_message(const char *buffer, char *sender_ip, char *sender_username, char *msg_type, char *content) {
    char *token;
    char *rest;
    char temp[BUFFER_SIZE];
    char sender_with_ip[BUFFER_SIZE];
    
    // Make a copy of the buffer since strtok_r modifies it
    strncpy(temp, buffer, BUFFER_SIZE - 1);
    temp[BUFFER_SIZE - 1] = '\0';
    
    // Parse message type
    token = strtok_r(temp, "|", &rest);
    if (token == NULL) {
        return -1;
    }
    strncpy(msg_type, token, 31);
    msg_type[31] = '\0';
    
    // Parse sender with IP
    token = strtok_r(NULL, "|", &rest);
    if (token == NULL) {
        return -1;
    }
    strncpy(sender_with_ip, token, BUFFER_SIZE - 1);
    
    // Extract IP from sender
    char *at_sign = strchr(sender_with_ip, '@');
    if (at_sign != NULL) {

        // Extract username
        int username_len = at_sign - sender_with_ip;
        strncpy(sender_username, sender_with_ip, username_len);
        sender_username[username_len] = '\0';

        // Extract IP
        strncpy(sender_ip, at_sign + 1, INET_ADDRSTRLEN - 1);
        sender_ip[INET_ADDRSTRLEN - 1] = '\0';
    }
    
    // Parse content
    token = strtok_r(NULL, "", &rest);
    if (token == NULL) {
        content[0] = '\0';
    } else {
        strncpy(content, token, BUFFER_SIZE - 1);
        content[BUFFER_SIZE - 1] = '\0';
    }
    
    return 0;
}