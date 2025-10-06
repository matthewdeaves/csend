/*
 * OpenTransport Messaging Interface
 * Full implementation using event-driven OpenTransport architecture
 */

#include "messaging.h"
#include "opentransport_impl.h"
#include "dialog_messages.h"
#include "../shared/logging.h"
#include "../shared/protocol.h"
#include "../shared/peer_wrapper.h"
#include <string.h>
#include <stdio.h>
#include <OSUtils.h>

/* Initialize messaging system */
OSErr InitMessaging(void)
{
    log_debug_cat(LOG_CAT_MESSAGING, "Messaging system initialized (OpenTransport event-driven)");
    return noErr;
}

/* Shutdown messaging system */
void ShutdownMessaging(void)
{
    log_debug_cat(LOG_CAT_MESSAGING, "Messaging system shutdown (OpenTransport event-driven)");
}

/* Send message to specific peer */
OSErr SendMessageToPeer(const char *targetIP, const char *message, const char *msg_type)
{
    char formattedMessage[BUFFER_SIZE];
    const char *username = GetUsername();
    char localIP[16];
    int result;

    if (!targetIP || !msg_type) {
        log_error_cat(LOG_CAT_MESSAGING, "SendMessageToPeer: Invalid parameters");
        return paramErr;
    }

    /* Get local IP address */
    GetLocalIPAddress(localIP, sizeof(localIP));

    /* Format message according to protocol */
    result = format_message(formattedMessage, sizeof(formattedMessage), msg_type,
                            generate_message_id(), username, localIP, message ? message : "");
    if (result <= 0) {
        log_error_cat(LOG_CAT_MESSAGING, "SendMessageToPeer: Failed to format message");
        return paramErr;
    }

    log_debug_cat(LOG_CAT_MESSAGING, "Sending TCP message (%s) to %s: %s", msg_type, targetIP, message ? message : "");

    /* Use our OpenTransport TCP implementation */
    return SendTCPMessage(formattedMessage, targetIP, TCP_PORT);
}

/* Broadcast message to all peers */
OSErr BroadcastMessage(const char *message)
{
    int sent_count = 0;
    int failed_count = 0;
    OSErr err;

    if (!message) {
        log_error_cat(LOG_CAT_MESSAGING, "BroadcastMessage: Invalid message");
        return paramErr;
    }

    log_debug_cat(LOG_CAT_MESSAGING, "Broadcasting message: %s", message);

    int active_count = pw_get_active_peer_count();

    for (int i = 0; i < active_count; i++) {
        peer_t peer;
        pw_get_peer_by_index(i, &peer);
        err = SendMessageToPeer(peer.ip, message, MSG_TEXT);
        if (err == noErr) {
            sent_count++;
        } else {
            failed_count++;
            log_error_cat(LOG_CAT_MESSAGING, "Failed to send broadcast message to %s", peer.ip);
        }
    }

    log_debug_cat(LOG_CAT_MESSAGING, "Broadcast complete: sent to %d peer(s), %d failed",
                  sent_count, failed_count);

    /* Return success if we sent to at least one peer, or noErr if no active peers */
    return (sent_count > 0 || (sent_count == 0 && failed_count == 0)) ? noErr : paramErr;
}

/* Send quit message */
OSErr BroadcastQuitMessage(void)
{
    char formattedMessage[BUFFER_SIZE];
    const char *username = GetUsername();
    char localIP[16];
    int result;

    /* Get local IP address */
    GetLocalIPAddress(localIP, sizeof(localIP));

    /* Format quit message according to protocol */
    result = format_message(formattedMessage, sizeof(formattedMessage), MSG_QUIT,
                            generate_message_id(), username, localIP, "");
    if (result <= 0) {
        log_error_cat(LOG_CAT_MESSAGING, "BroadcastQuitMessage: Failed to format message");
        return paramErr;
    }

    log_debug_cat(LOG_CAT_MESSAGING, "Broadcasting quit message");

    /* Use our OpenTransport UDP implementation */
    return SendUDPMessage(formattedMessage, "255.255.255.255", UDP_PORT);
}

/* Process incoming TCP message (called by OpenTransport TCP event handlers) */
void ProcessIncomingMessage(const char *rawMessage, const char *senderIP)
{
    char message[BUFFER_SIZE];
    char sender_username[32];
    char sender_ip[INET_ADDRSTRLEN];
    char msg_type[32];
    int messageLen;
    char myLocalIP[16];

    if (!rawMessage || !senderIP) {
        return;
    }

    /* Get our local IP to filter out our own messages */
    GetLocalIPAddress(myLocalIP, sizeof(myLocalIP));

    /* Ignore messages from ourselves */
    if (strcmp(senderIP, myLocalIP) == 0) {
        log_debug_cat(LOG_CAT_MESSAGING, "Ignored TCP message from self (%s)", senderIP);
        return;
    }

    messageLen = strlen(rawMessage);
    log_debug_cat(LOG_CAT_MESSAGING, "Processing TCP message from %s: %s", senderIP, rawMessage);

    /* Parse the protocol message */
    csend_uint32_t msg_id;
    if (parse_message(rawMessage, messageLen, sender_ip, sender_username, msg_type, &msg_id, message) == 0) {
        log_debug_cat(LOG_CAT_MESSAGING, "Received message ID %lu from %s@%s",
                      (unsigned long)msg_id, sender_username, senderIP);

        if (strcmp(msg_type, MSG_TEXT) == 0) {
            log_debug_cat(LOG_CAT_MESSAGING, "Text message from %s: %s", sender_username, message);
            /* Add peer and display message */
            AddOrUpdatePeer(senderIP, sender_username);

            /* Display in UI */
            char displayMsg[BUFFER_SIZE + 100];
            snprintf(displayMsg, sizeof(displayMsg), "%s: %s\r", sender_username, message);
            AppendToMessagesTE(displayMsg);
        } else {
            log_warning_cat(LOG_CAT_MESSAGING, "Unexpected message type '%s' on TCP from %s (should be TEXT)", msg_type, sender_username);
        }
    } else {
        log_error_cat(LOG_CAT_MESSAGING, "Failed to parse TCP message: %s", rawMessage);
    }
}