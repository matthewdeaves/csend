#include "messaging.h"
#include "protocol.h"
#include "logging.h"
#include <string.h>
void handle_received_tcp_message(const char *sender_ip,
                                 const char *sender_username,
                                 const char *msg_type,
                                 const char *content,
                                 const tcp_platform_callbacks_t *callbacks,
                                 void *platform_context)
{
    int add_result;
    if (!callbacks || !callbacks->add_or_update_peer || !callbacks->display_text_message || !callbacks->mark_peer_inactive) {
        log_debug("Error (handle_received_tcp_message): Invalid or incomplete callbacks provided.");
        return;
    }
    add_result = callbacks->add_or_update_peer(sender_ip, sender_username, platform_context);
    if (add_result > 0) {
        log_debug("Shared TCP Handler: New peer added/updated via TCP: %s@%s", sender_username, sender_ip);
    } else if (add_result == 0) {
        log_debug("Shared TCP Handler: Existing peer updated via TCP: %s@%s", sender_username, sender_ip);
    } else {
        log_debug("Shared TCP Handler: Peer list full or error adding/updating %s@%s from TCP.", sender_username, sender_ip);
    }
    log_debug("Shared TCP Handler: Processing message type '%s' from %s@%s", msg_type, sender_username, sender_ip);
    if (strcmp(msg_type, MSG_TEXT) == 0) {
        log_debug("Shared TCP Handler: Calling display_text_message callback.");
        callbacks->display_text_message(sender_username, sender_ip, content, platform_context);
    } else if (strcmp(msg_type, MSG_QUIT) == 0) {
        log_debug("Shared TCP Handler: Received QUIT from %s@%s. Calling mark_peer_inactive callback.", sender_username, sender_ip);
        callbacks->mark_peer_inactive(sender_ip, platform_context);
    } else {
        log_debug("Shared TCP Handler: Received unhandled TCP message type '%s' from %s@%s.", msg_type, sender_username, sender_ip);
    }
}
