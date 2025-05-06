#ifndef DISCOVERY_LOGIC_H
#define DISCOVERY_LOGIC_H
#include "common_defs.h"
#include <stdint.h>
typedef struct {
    void (*send_response_callback)(uint32_t dest_ip_addr, uint16_t dest_port, void *platform_context);
    int (*add_or_update_peer_callback)(const char *ip, const char *username, void *platform_context);
    void (*notify_peer_list_updated_callback)(void *platform_context);
} discovery_platform_callbacks_t;
void discovery_logic_process_packet(const char *buffer, int len,
                                    const char *sender_ip_str, uint32_t sender_ip_addr, uint16_t sender_port,
                                    const discovery_platform_callbacks_t *callbacks,
                                    void *platform_context);
#endif
