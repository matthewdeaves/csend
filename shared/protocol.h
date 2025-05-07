#ifndef PROTOCOL_H
#define PROTOCOL_H 
#include "common_defs.h"
#ifdef __MACOS__
#include <MacTypes.h>
#else
#include <stdint.h>
#endif
#define MSG_MAGIC_NUMBER 0x43534443UL
#define MSG_DISCOVERY "DISCOVERY"
#define MSG_DISCOVERY_RESPONSE "DISCOVERY_RESPONSE"
#define MSG_TEXT "TEXT"
#define MSG_QUIT "QUIT"
int format_message(char *buffer, int buffer_size, const char *msg_type,
                   const char *sender, const char *local_ip_str, const char *content);
int parse_message(const char *buffer, int buffer_len, char *sender_ip, char *sender_username,
                  char *msg_type, char *content);
#endif
