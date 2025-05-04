#ifndef MESSAGING_LOGIC_H
#define MESSAGING_LOGIC_H 
#include "common_defs.h"
typedef struct {
    int (*add_or_update_peer)(const char* ip, const char* username, void* platform_context);
    void (*display_text_message)(const char* username, const char* ip, const char* message_content, void* platform_context);
    void (*mark_peer_inactive)(const char* ip, void* platform_context);
} tcp_platform_callbacks_t;
void handle_received_tcp_message(const char* sender_ip,
                                 const char* sender_username,
                                 const char* msg_type,
                                 const char* content,
                                 const tcp_platform_callbacks_t* callbacks,
                                 void* platform_context);
#endif
