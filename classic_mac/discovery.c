//====================================
// FILE: ./classic_mac/discovery.c
//====================================

#include "discovery.h"
#include "network_abstraction.h"
#include "../shared/logging.h"
#include "../shared/logging.h"
#include "protocol.h"
#include "../shared/discovery.h"
#include "peer.h"
#include "network_init.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include <Errors.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Memory.h>
#include <stddef.h>
#include <OSUtils.h>

/* Forward declarations for internal functions */
static OSErr StartAsyncUDPRead(void);
static OSErr ReturnUDPBufferAsync(Ptr dataPtr, unsigned short bufferSize);

/* Internal state - no MacTCP types exposed */
static NetworkEndpointRef gUDPEndpoint = NULL;
static Ptr gUDPRecvBuffer = NULL;
static NetworkAsyncHandle gUDPReadHandle = NULL;
static NetworkAsyncHandle gUDPReturnHandle = NULL;
static NetworkAsyncHandle gUDPSendHandle = NULL;
static unsigned long gLastBroadcastTimeTicks = 0;

/* Buffers for messages */
static char gBroadcastBuffer[BUFFER_SIZE];
static char gResponseBuffer[BUFFER_SIZE];

/* Platform callbacks */
static void mac_send_discovery_response(uint32_t dest_ip_addr_host_order, uint16_t dest_port_host_order, void *platform_context)
{
    (void)platform_context;
    ip_addr dest_ip_net = (ip_addr)dest_ip_addr_host_order;
    udp_port dest_port_mac = dest_port_host_order;

    /* Note: Responses are sent directly without queuing since they're small and infrequent */
    OSErr sendErr = SendDiscoveryResponseSync(gMacTCPRefNum, gMyUsername, gMyLocalIPStr, dest_ip_net, dest_port_mac);
    if (sendErr != noErr && sendErr != 1) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error sending discovery response: %d to IP 0x%lX:%u", sendErr, (unsigned long)dest_ip_net, dest_port_mac);
    } else if (sendErr == 1) {
        log_debug_cat(LOG_CAT_DISCOVERY, "Discovery response skipped - send already pending");
    } else {
        char tempIPStr[INET_ADDRSTRLEN];
        if (gNetworkOps && gNetworkOps->AddressToString) {
            gNetworkOps->AddressToString(dest_ip_net, tempIPStr);
        } else {
            sprintf(tempIPStr, "%lu.%lu.%lu.%lu",
                    (dest_ip_net >> 24) & 0xFF, (dest_ip_net >> 16) & 0xFF,
                    (dest_ip_net >> 8) & 0xFF, dest_ip_net & 0xFF);
        }
        log_debug_cat(LOG_CAT_DISCOVERY, "Sent DISCOVERY_RESPONSE to %s:%u", tempIPStr, dest_port_mac);
    }
}

static int mac_add_or_update_peer(const char *ip, const char *username, void *platform_context)
{
    (void)platform_context;
    return AddOrUpdatePeer(ip, username);
}

static void mac_notify_peer_list_updated(void *platform_context)
{
    (void)platform_context;
    if (gMainWindow != NULL && gPeerListHandle != NULL) {
        UpdatePeerDisplayList(true);
    }
}

static void mac_mark_peer_inactive(const char *ip, void *platform_context)
{
    (void)platform_context;
    MarkPeerInactive(ip);
}

OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum)
{
    OSErr err;

    log_info_cat(LOG_CAT_DISCOVERY, "Initializing UDP Discovery Endpoint using network abstraction...");

    if (!gNetworkOps) {
        log_app_event("Error: Network abstraction not initialized");
        return notOpenErr;
    }

    if (macTCPRefNum == 0) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error (InitUDP): macTCPRefNum is 0.");
        return paramErr;
    }

    /* Clean up any previous state */
    gUDPEndpoint = NULL;
    gUDPRecvBuffer = NULL;
    gUDPReadHandle = NULL;
    gUDPReturnHandle = NULL;
    gUDPSendHandle = NULL;
    gLastBroadcastTimeTicks = 0;

    /* Allocate receive buffer */
    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);
    if (gUDPRecvBuffer == NULL) {
        log_app_event("Fatal Error: Could not allocate UDP receive buffer (%ld bytes).", (long)kMinUDPBufSize);
        return memFullErr;
    }
    log_debug_cat(LOG_CAT_DISCOVERY, "Allocated %ld bytes for UDP receive buffer at 0x%lX.", (long)kMinUDPBufSize, (unsigned long)gUDPRecvBuffer);

    /* Create UDP endpoint using abstraction */
    err = gNetworkOps->UDPCreate(macTCPRefNum, &gUDPEndpoint, PORT_UDP, gUDPRecvBuffer, kMinUDPBufSize);
    if (err != noErr || gUDPEndpoint == NULL) {
        log_app_event("Error (InitUDP): UDPCreate failed (Error: %d).", err);
        DisposePtr(gUDPRecvBuffer);
        gUDPRecvBuffer = NULL;
        return err;
    }

    log_info_cat(LOG_CAT_DISCOVERY, "UDP Endpoint created successfully using network abstraction on port %u.", PORT_UDP);

    /* Start initial async read */
    err = StartAsyncUDPRead();
    if (err != noErr && err != 1) {
        log_app_event("Error (InitUDP): Failed to start initial async UDP read. Error: %d", err);
        CleanupUDPDiscoveryEndpoint(macTCPRefNum);
        return err;
    }

    log_debug_cat(LOG_CAT_DISCOVERY, "Initial asynchronous UDP read started.");
    return noErr;
}

void CleanupUDPDiscoveryEndpoint(short macTCPRefNum)
{
    OSErr err;

    log_debug_cat(LOG_CAT_DISCOVERY, "Cleaning up UDP Discovery Endpoint...");

    /* Cancel any pending async operations */
    if (gUDPReadHandle != NULL && gNetworkOps && gNetworkOps->UDPCancelAsync) {
        log_debug_cat(LOG_CAT_DISCOVERY, "Cancelling pending UDP read operation...");
        gNetworkOps->UDPCancelAsync(gUDPReadHandle);
        gUDPReadHandle = NULL;
    }

    if (gUDPReturnHandle != NULL && gNetworkOps && gNetworkOps->UDPCancelAsync) {
        log_debug_cat(LOG_CAT_DISCOVERY, "Cancelling pending UDP buffer return operation...");
        gNetworkOps->UDPCancelAsync(gUDPReturnHandle);
        gUDPReturnHandle = NULL;
    }

    if (gUDPSendHandle != NULL && gNetworkOps && gNetworkOps->UDPCancelAsync) {
        log_debug_cat(LOG_CAT_DISCOVERY, "Cancelling pending UDP send operation...");
        gNetworkOps->UDPCancelAsync(gUDPSendHandle);
        gUDPSendHandle = NULL;
    }

    /* Release UDP endpoint */
    if (gUDPEndpoint != NULL && gNetworkOps && gNetworkOps->UDPRelease) {
        log_debug_cat(LOG_CAT_DISCOVERY, "Releasing UDP endpoint...");
        err = gNetworkOps->UDPRelease(macTCPRefNum, gUDPEndpoint);
        if (err != noErr) {
            log_warning_cat(LOG_CAT_DISCOVERY, "UDPRelease failed during cleanup (Error: %d).", err);
        }
        gUDPEndpoint = NULL;
    }

    /* Dispose receive buffer */
    if (gUDPRecvBuffer != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
        DisposePtr(gUDPRecvBuffer);
        gUDPRecvBuffer = NULL;
    }

    gLastBroadcastTimeTicks = 0;
    log_debug_cat(LOG_CAT_DISCOVERY, "UDP Discovery Endpoint cleanup finished.");
}

OSErr SendDiscoveryBroadcastSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr)
{
    OSErr err;
    int formatted_len;

    (void)macTCPRefNum; /* Not needed with abstraction */

    if (!gNetworkOps || gUDPEndpoint == NULL) return notOpenErr;
    if (myUsername == NULL || myLocalIPStr == NULL) return paramErr;

    /* Check if a send is already pending */
    if (gUDPSendHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "SendDiscoveryBroadcastSync: Send already pending, skipping");
        return 1;  /* Indicate busy */
    }

    log_debug_cat(LOG_CAT_DISCOVERY, "Sending Discovery Broadcast...");

    /* Format the message */
    formatted_len = format_message(gBroadcastBuffer, BUFFER_SIZE, MSG_DISCOVERY,
                                   generate_message_id(), myUsername, myLocalIPStr, "");
    if (formatted_len <= 0) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error: format_message failed for DISCOVERY");
        return paramErr;
    }

    /* Send using async abstraction layer */
    if (!gNetworkOps->UDPSendAsync) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error: Async UDP send not available");
        return notOpenErr;
    }

    err = gNetworkOps->UDPSendAsync(gUDPEndpoint, BROADCAST_IP, PORT_UDP,
                                    (Ptr)gBroadcastBuffer, formatted_len - 1,
                                    &gUDPSendHandle);
    if (err != noErr) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error starting async broadcast: %d", err);
        gUDPSendHandle = NULL;
    } else {
        log_debug_cat(LOG_CAT_DISCOVERY, "Broadcast send initiated asynchronously");
    }

    return err;
}

OSErr SendDiscoveryResponseSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr,
                                ip_addr destIP, udp_port destPort)
{
    OSErr err;
    int formatted_len;

    (void)macTCPRefNum; /* Not needed with abstraction */

    if (!gNetworkOps || gUDPEndpoint == NULL) return notOpenErr;
    if (myUsername == NULL || myLocalIPStr == NULL) return paramErr;

    /* Check if a send is already pending */
    if (gUDPSendHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "SendDiscoveryResponseSync: Send already pending, skipping response");
        return 1;  /* Indicate busy */
    }

    log_debug_cat(LOG_CAT_DISCOVERY, "Sending Discovery Response to IP 0x%lX:%u...", (unsigned long)destIP, destPort);

    /* Format the message */
    formatted_len = format_message(gResponseBuffer, BUFFER_SIZE, MSG_DISCOVERY_RESPONSE,
                                   generate_message_id(), myUsername, myLocalIPStr, "");
    if (formatted_len <= 0) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error: format_message failed for DISCOVERY_RESPONSE");
        return paramErr;
    }

    /* Send using async abstraction layer */
    if (!gNetworkOps->UDPSendAsync) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error: Async UDP send not available");
        return notOpenErr;
    }

    err = gNetworkOps->UDPSendAsync(gUDPEndpoint, destIP, destPort,
                                    (Ptr)gResponseBuffer, formatted_len - 1,
                                    &gUDPSendHandle);
    if (err != noErr) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error starting async response: %d", err);
        gUDPSendHandle = NULL;
    } else {
        log_debug_cat(LOG_CAT_DISCOVERY, "Response send initiated asynchronously");
    }

    return err;
}

OSErr BroadcastQuitMessage(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr)
{
    OSErr err;
    int formatted_len;
    static char quitBuffer[BUFFER_SIZE];

    (void)macTCPRefNum; /* Not needed with abstraction */

    if (!gNetworkOps || gUDPEndpoint == NULL) return notOpenErr;
    if (myUsername == NULL || myLocalIPStr == NULL) return paramErr;

    /* Check if a send is already pending - wait briefly if needed */
    if (gUDPSendHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "BroadcastQuitMessage: Send pending, waiting briefly...");
        unsigned long startTime = TickCount();
        while (gUDPSendHandle != NULL && (TickCount() - startTime) < 60) {  /* Wait up to 1 second */
            if (gNetworkOps->UDPCheckSendStatus) {
                OSErr status = gNetworkOps->UDPCheckSendStatus(gUDPSendHandle);
                if (status != 1) {  /* Not pending anymore */
                    gUDPSendHandle = NULL;
                    break;
                }
            }
            YieldTimeToSystem();
        }
        if (gUDPSendHandle != NULL) {
            log_warning_cat(LOG_CAT_DISCOVERY, "BroadcastQuitMessage: Previous send still pending, sending anyway");
            gUDPSendHandle = NULL;  /* Force clear to allow quit message */
        }
    }

    log_info_cat(LOG_CAT_DISCOVERY, "Broadcasting quit message");

    /* Format the quit message */
    formatted_len = format_message(quitBuffer, BUFFER_SIZE, MSG_QUIT,
                                   generate_message_id(), myUsername, myLocalIPStr, "");
    if (formatted_len <= 0) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error: format_message failed for MSG_QUIT");
        return paramErr;
    }

    /* Send using async abstraction layer */
    if (!gNetworkOps->UDPSendAsync) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error: Async UDP send not available");
        return notOpenErr;
    }

    err = gNetworkOps->UDPSendAsync(gUDPEndpoint, BROADCAST_IP, PORT_UDP,
                                    (Ptr)quitBuffer, formatted_len - 1,
                                    &gUDPSendHandle);
    if (err != noErr) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error broadcasting quit message: %d", err);
        gUDPSendHandle = NULL;
    } else {
        log_debug_cat(LOG_CAT_DISCOVERY, "Quit broadcast initiated asynchronously");
    }

    return err;
}

static OSErr StartAsyncUDPRead(void)
{
    OSErr err;

    if (!gNetworkOps || !gNetworkOps->UDPReceiveAsync) return notOpenErr;
    if (gUDPEndpoint == NULL) return invalidStreamPtr;

    if (gUDPReadHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "StartAsyncUDPRead: UDP read already pending. Ignoring request.");
        return 1;
    }

    if (gUDPReturnHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "StartAsyncUDPRead: Cannot start new read, buffer return is pending. Try later.");
        return 1;
    }

    if (gUDPRecvBuffer == NULL) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error (StartAsyncUDPRead): gUDPRecvBuffer is NULL.");
        return invalidBufPtr;
    }

    /* Start async receive using abstraction */
    err = gNetworkOps->UDPReceiveAsync(gUDPEndpoint, &gUDPReadHandle);
    if (err != noErr) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error (StartAsyncUDPRead): UDPReceiveAsync failed. Error: %d", err);
        gUDPReadHandle = NULL;
        return err;
    }

    log_debug_cat(LOG_CAT_DISCOVERY, "StartAsyncUDPRead: Async UDP read initiated.");
    return noErr;
}

static OSErr ReturnUDPBufferAsync(Ptr dataPtr, unsigned short bufferSize)
{
    OSErr err;

    if (!gNetworkOps || !gNetworkOps->UDPReturnBufferAsync || gUDPEndpoint == NULL) return invalidStreamPtr;

    if (gUDPReturnHandle != NULL) {
        log_debug_cat(LOG_CAT_DISCOVERY, "ReturnUDPBufferAsync: Buffer return already pending. Ignoring request.");
        return 1;
    }

    if (dataPtr == NULL) {
        log_error_cat(LOG_CAT_DISCOVERY, "Error (ReturnUDPBufferAsync): dataPtr is NULL. Cannot return.");
        return invalidBufPtr;
    }

    /* Return buffer using abstraction */
    err = gNetworkOps->UDPReturnBufferAsync(gUDPEndpoint, dataPtr, bufferSize, &gUDPReturnHandle);
    if (err != noErr) {
        log_error_cat(LOG_CAT_DISCOVERY, "CRITICAL Error (ReturnUDPBufferAsync): UDPReturnBufferAsync failed. Error: %d.", err);
        gUDPReturnHandle = NULL;
        return err;
    }

    log_debug_cat(LOG_CAT_DISCOVERY, "ReturnUDPBufferAsync: Async buffer return initiated for buffer 0x%lX.", (unsigned long)dataPtr);
    return noErr;
}

void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr)
{
    unsigned long currentTimeTicks = TickCount();
    const unsigned long intervalTicks = (unsigned long)DISCOVERY_INTERVAL * 60UL;

    if (gUDPEndpoint == NULL || !gNetworkOps) return;

    if (currentTimeTicks < gLastBroadcastTimeTicks) {
        gLastBroadcastTimeTicks = currentTimeTicks;
    }

    if (gLastBroadcastTimeTicks == 0 || (currentTimeTicks - gLastBroadcastTimeTicks) >= intervalTicks) {
        log_debug_cat(LOG_CAT_DISCOVERY, "CheckSendBroadcast: Interval elapsed. Sending broadcast.");
        OSErr sendErr = SendDiscoveryBroadcastSync(macTCPRefNum, myUsername, myLocalIPStr);
        if (sendErr == noErr) {
            gLastBroadcastTimeTicks = currentTimeTicks;
        } else {
            log_error_cat(LOG_CAT_DISCOVERY, "Sync broadcast initiation FAILED (Error: %d). Will retry next interval.", sendErr);
        }
    }
}

void PollUDPListener(short macTCPRefNum, ip_addr myLocalIP)
{
    OSErr status;
    ip_addr remoteHost;
    udp_port remotePort;
    Ptr dataPtr;
    unsigned short dataLength;

    static discovery_platform_callbacks_t mac_callbacks = {
        .send_response_callback = mac_send_discovery_response,
        .add_or_update_peer_callback = mac_add_or_update_peer,
        .notify_peer_list_updated_callback = mac_notify_peer_list_updated,
        .mark_peer_inactive_callback = mac_mark_peer_inactive
    };

    (void)macTCPRefNum; /* Not needed with abstraction */

    if (!gNetworkOps || gUDPEndpoint == NULL) return;

    /* Check async read status */
    if (gUDPReadHandle != NULL && gNetworkOps->UDPCheckAsyncStatus) {
        status = gNetworkOps->UDPCheckAsyncStatus(gUDPReadHandle, &remoteHost, &remotePort,
                 &dataPtr, &dataLength);

        if (status == 1) {
            /* Still pending, nothing to do */
        } else if (status == noErr) {
            /* Read completed successfully */
            gUDPReadHandle = NULL;

            if (dataLength > 0) {
                if (remoteHost != myLocalIP) {
                    char senderIPStr[INET_ADDRSTRLEN];
                    if (gNetworkOps->AddressToString) {
                        OSErr addrErr = gNetworkOps->AddressToString(remoteHost, senderIPStr);
                        if (addrErr != noErr) {
                            sprintf(senderIPStr, "%lu.%lu.%lu.%lu",
                                    (remoteHost >> 24) & 0xFF, (remoteHost >> 16) & 0xFF,
                                    (remoteHost >> 8) & 0xFF, remoteHost & 0xFF);
                        }
                    } else {
                        sprintf(senderIPStr, "%lu.%lu.%lu.%lu",
                                (remoteHost >> 24) & 0xFF, (remoteHost >> 16) & 0xFF,
                                (remoteHost >> 8) & 0xFF, remoteHost & 0xFF);
                    }

                    uint32_t sender_ip_for_shared = (uint32_t)remoteHost;
                    discovery_logic_process_packet((const char *)dataPtr, dataLength,
                                                   senderIPStr, sender_ip_for_shared, remotePort,
                                                   &mac_callbacks, NULL);
                } else {
                    char selfIPStr[INET_ADDRSTRLEN];
                    if (gNetworkOps->AddressToString) {
                        gNetworkOps->AddressToString(remoteHost, selfIPStr);
                    } else {
                        sprintf(selfIPStr, "%lu.%lu.%lu.%lu",
                                (remoteHost >> 24) & 0xFF, (remoteHost >> 16) & 0xFF,
                                (remoteHost >> 8) & 0xFF, remoteHost & 0xFF);
                    }
                    log_debug_cat(LOG_CAT_DISCOVERY, "PollUDPListener: Ignored UDP packet from self (%s).", selfIPStr);
                }

                /* Return the buffer asynchronously */
                OSErr returnErr = ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize);
                if (returnErr != noErr && returnErr != 1) {
                    log_error_cat(LOG_CAT_DISCOVERY, "CRITICAL Error: Failed to initiate async buffer return. Error: %d", returnErr);
                } else {
                    log_debug_cat(LOG_CAT_DISCOVERY, "PollUDPListener: Initiated return for buffer 0x%lX.", (unsigned long)dataPtr);
                }
            } else {
                log_debug_cat(LOG_CAT_DISCOVERY, "DEBUG: Async UDP read returned noErr but 0 bytes. Returning buffer.");
                (void)ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize);
            }
        } else {
            /* Error occurred */
            gUDPReadHandle = NULL;
            log_error_cat(LOG_CAT_DISCOVERY, "Error (PollUDPListener): Async UDP read completed with error: %d", status);

            /* Try to return buffer if possible */
            if (dataPtr != NULL) {
                (void)ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize);
            }
        }
    }

    /* Check buffer return completion */
    if (gUDPReturnHandle != NULL && gNetworkOps->UDPCheckReturnStatus) {
        status = gNetworkOps->UDPCheckReturnStatus(gUDPReturnHandle);

        if (status == 1) {
            /* Still pending */
        } else if (status == noErr) {
            /* Return completed successfully */
            gUDPReturnHandle = NULL;
            log_debug_cat(LOG_CAT_DISCOVERY, "PollUDPListener: Async buffer return completed successfully.");

            /* Start a new read if none pending */
            if (gUDPReadHandle == NULL && gUDPEndpoint != NULL) {
                StartAsyncUDPRead();
            }
        } else {
            /* Error occurred */
            gUDPReturnHandle = NULL;
            log_error_cat(LOG_CAT_DISCOVERY, "CRITICAL Error: Async buffer return completed with error: %d.", status);
        }
    }

    /* Check UDP send completion */
    if (gUDPSendHandle != NULL && gNetworkOps && gNetworkOps->UDPCheckSendStatus) {
        status = gNetworkOps->UDPCheckSendStatus(gUDPSendHandle);

        if (status == 1) {
            /* Still pending */
        } else if (status == noErr) {
            /* Send completed successfully */
            gUDPSendHandle = NULL;
            log_debug_cat(LOG_CAT_DISCOVERY, "PollUDPListener: UDP send completed successfully");
        } else {
            /* Error occurred */
            gUDPSendHandle = NULL;
            log_error_cat(LOG_CAT_DISCOVERY, "PollUDPListener: UDP send completed with error: %d", status);
        }
    }

    /* Ensure we always have a read pending */
    if (gUDPReadHandle == NULL && gUDPReturnHandle == NULL && gUDPEndpoint != NULL) {
        OSErr startErr = StartAsyncUDPRead();
        if (startErr != noErr && startErr != 1) {
            log_error_cat(LOG_CAT_DISCOVERY, "PollUDPListener: Failed to start new UDP read in idle fallback. Error: %d", startErr);
        }
    }
}