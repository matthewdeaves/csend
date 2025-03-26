// protocol.h
#ifndef PROTOCOL_H
#define PROTOCOL_H

// Function declarations
int format_message(char *buffer, int buffer_size, const char *msg_type, 
                  const char *sender, const char *content);
int parse_message(const char *buffer, char *sender_ip, char *sender_username, 
                 char *msg_type, char *content);

#endif // PROTOCOL_H