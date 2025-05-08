#include "discovery.h"
#include "protocol.h"
#include "logging.h"
#include <string.h>
void discovery_logic_process_packet(const char *buffer, int len,
                                    const char *sender_ip_str, uint32_t sender_ip_addr, uint16_t sender_port,
                                    const discovery_platform_callbacks_t *callbacks,
                                    void *platform_context)
{
    char sender_ip_from_payload[INET_ADDRSTRLEN];
    char sender_username[32];
    char msg_type[32];
    char content[BUFFER_SIZE];
    int add_result;
    if (!callbacks || !callbacks->add_or_update_peer_callback || !callbacks->send_response_callback || !callbacks->notify_peer_list_updated_callback) {
        log_message("Error (discovery_logic): Invalid callbacks provided.");
        return;
    }
    if (parse_message(buffer, len, sender_ip_from_payload, sender_username, msg_type, content) != 0) {
        log_message("Discarding invalid/unknown UDP msg from %s (%d bytes) - parse failed.", sender_ip_str, len);
        return;
    }
    if (strcmp(msg_type, MSG_DISCOVERY) == 0) {
        log_message("Received DISCOVERY from %s@%s", sender_username, sender_ip_str);
        callbacks->send_response_callback(sender_ip_addr, sender_port, platform_context);
        add_result = callbacks->add_or_update_peer_callback(sender_ip_str, sender_username, platform_context);
        if (add_result > 0) {
            log_message("New peer added via DISCOVERY: %s@%s", sender_username, sender_ip_str);
            callbacks->notify_peer_list_updated_callback(platform_context);
        } else if (add_result == 0) {
            log_message("Existing peer updated via DISCOVERY: %s@%s", sender_username, sender_ip_str);
        } else {
            log_message("Peer list full, could not add %s@%s from DISCOVERY", sender_username, sender_ip_str);
        }
    } else if (strcmp(msg_type, MSG_DISCOVERY_RESPONSE) == 0) {
        log_message("Received DISCOVERY_RESPONSE from %s@%s", sender_username, sender_ip_str);
        add_result = callbacks->add_or_update_peer_callback(sender_ip_str, sender_username, platform_context);
        if (add_result > 0) {
            log_message("New peer added via RESPONSE: %s@%s", sender_username, sender_ip_str);
            callbacks->notify_peer_list_updated_callback(platform_context);
        } else if (add_result == 0) {
            log_message("Existing peer updated via RESPONSE: %s@%s", sender_username, sender_ip_str);
        } else {
            log_message("Peer list full, could not add %s@%s from RESPONSE", (sender_username[0] != '\0') ? sender_username : "??", sender_ip_str);
        }
    } else {
        log_message("Received unhandled UDP message type '%s' from %s@%s.", msg_type, sender_username, sender_ip_str);
    }
}
