/*
 * OpenTransport Discovery Interface
 * Full implementation using event-driven OpenTransport architecture
 */

#include "discovery.h"
#include "opentransport_impl.h"
#include "dialog_peerlist.h"
#include "../shared/logging.h"
#include "../shared/protocol.h"
#include "../shared/discovery.h"
#include "../shared/peer_wrapper.h"
#include <stdio.h>
#include <string.h>
#include <OSUtils.h>
#include <OpenTptInternet.h>

/* Global state */
static Boolean gDiscoveryInitialized = false;
static unsigned long gLastDiscoveryTime = 0;

/* Discovery interval (ticks) */
#define DISCOVERY_INTERVAL_TICKS (5 * 60)  /* 5 seconds at 60 Hz */

/* Platform-specific callbacks for shared discovery logic */
static void ot_send_discovery_response(uint32_t dest_ip_addr, uint16_t dest_port, void *platform_context)
{
    char formattedMessage[BUFFER_SIZE];
    const char *username = GetUsername();
    char localIP[16];
    int result;
    char destIPStr[16];

    (void)platform_context; /* Unused */

    /* Get local IP address */
    GetLocalIPAddress(localIP, sizeof(localIP));

    /* Convert dest IP to string */
    OTInetHostToString(dest_ip_addr, destIPStr);

    /* Format discovery response message */
    result = format_message(formattedMessage, sizeof(formattedMessage), MSG_DISCOVERY_RESPONSE,
                            generate_message_id(), username, localIP, "");
    if (result <= 0) {
        log_error_cat(LOG_CAT_DISCOVERY, "Failed to format discovery response");
        return;
    }

    log_debug_cat(LOG_CAT_DISCOVERY, "Sending discovery response to %s", destIPStr);
    SendUDPMessage(formattedMessage, destIPStr, dest_port);
}

static int ot_add_or_update_peer(const char *ip, const char *username, void *platform_context)
{
    (void)platform_context; /* Unused */
    return AddOrUpdatePeer(ip, username);
}

static void ot_notify_peer_list_updated(void *platform_context)
{
    (void)platform_context; /* Unused */
    /* UI updates happen automatically on Mac via UpdatePeerDisplayList() */
    UpdatePeerDisplayList(true);
}

static void ot_mark_peer_inactive(const char *ip, void *platform_context)
{
    (void)platform_context; /* Unused */
    MarkPeerInactive(ip);
}

/* Initialize discovery system */
OSErr InitDiscovery(void)
{
    log_debug_cat(LOG_CAT_DISCOVERY, "Discovery system initialized (OpenTransport event-driven)");
    gDiscoveryInitialized = true;
    return noErr;
}

/* Shutdown discovery system */
void ShutdownDiscovery(void)
{
    log_debug_cat(LOG_CAT_DISCOVERY, "Discovery system shutdown (OpenTransport event-driven)");
    gDiscoveryInitialized = false;
}

/* Process discovery - called periodically */
void ProcessDiscovery(void)
{
    unsigned long currentTime = TickCount();

    if (!gDiscoveryInitialized) {
        return;
    }

    /* Send discovery broadcast periodically */
    if (gLastDiscoveryTime == 0 ||
            (currentTime >= gLastDiscoveryTime &&
             (currentTime - gLastDiscoveryTime) >= DISCOVERY_INTERVAL_TICKS)) {

        SendDiscoveryBroadcast();
        gLastDiscoveryTime = currentTime;
    }
}

/* Send discovery broadcast */
OSErr SendDiscoveryBroadcast(void)
{
    char formattedMessage[BUFFER_SIZE];
    const char *username = GetUsername();
    char localIP[16];
    int result;

    if (!gDiscoveryInitialized) {
        return -1;
    }

    /* Get local IP address */
    GetLocalIPAddress(localIP, sizeof(localIP));

    /* Format discovery message according to protocol */
    result = format_message(formattedMessage, sizeof(formattedMessage), MSG_DISCOVERY,
                            generate_message_id(), username, localIP, "");
    if (result <= 0) {
        log_error_cat(LOG_CAT_DISCOVERY, "SendDiscoveryBroadcast: Failed to format message");
        return paramErr;
    }

    log_debug_cat(LOG_CAT_DISCOVERY, "Sending discovery broadcast");

    /* Use our OpenTransport UDP implementation */
    return SendUDPMessage(formattedMessage, "255.255.255.255", UDP_PORT);
}

/* Process incoming UDP discovery message */
void ProcessIncomingUDPMessage(const char *buffer, int len, const char *senderIPStr, UInt32 senderIPAddr, UInt16 senderPort)
{
    static discovery_platform_callbacks_t callbacks = {
        .send_response_callback = ot_send_discovery_response,
        .add_or_update_peer_callback = ot_add_or_update_peer,
        .notify_peer_list_updated_callback = ot_notify_peer_list_updated,
        .mark_peer_inactive_callback = ot_mark_peer_inactive
    };

    /* Use shared discovery logic */
    discovery_logic_process_packet(buffer, len, senderIPStr, senderIPAddr, senderPort, &callbacks, NULL);
}