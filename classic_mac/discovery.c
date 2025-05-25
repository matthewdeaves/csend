#include "discovery.h"
#include "network_abstraction.h"  /* Add this */
#include "logging.h"
#include "../shared/logging.h"
#include "protocol.h"
#include "../shared/discovery.h"
#include "peer.h"
#include "network_init.h"
#include "dialog.h"
#include "dialog_peerlist.h"
#include <Devices.h>
#include <Errors.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Memory.h>
#include <stddef.h>
#include <MacTCP.h>
#include <OSUtils.h>

/* Need access to the internal structure for async operations */
typedef struct {
    StreamPtr stream;
    udp_port localPort;
    Ptr recvBuffer;
    unsigned short bufferSize;
    Boolean isCreated;
} MacTCPUDPEndpoint;

/* Change from StreamPtr to NetworkEndpointRef - internal only */
static NetworkEndpointRef gUDPEndpoint = NULL;

/* These are extern in the header, so not static here */
Ptr gUDPRecvBuffer = NULL;
UDPiopb gUDPReadPB;
UDPiopb gUDPBfrReturnPB;
Boolean gUDPReadPending = false;
Boolean gUDPBfrReturnPending = false;
unsigned long gLastBroadcastTimeTicks = 0;

static char gBroadcastBuffer[BUFFER_SIZE];
static char gResponseBuffer[BUFFER_SIZE];

/* Platform callbacks */
static void mac_send_discovery_response(uint32_t dest_ip_addr_host_order, uint16_t dest_port_host_order, void *platform_context)
{
    (void)platform_context;
    ip_addr dest_ip_net = (ip_addr)dest_ip_addr_host_order;
    udp_port dest_port_mac = dest_port_host_order;

    OSErr sendErr = SendDiscoveryResponseSync(gMacTCPRefNum, gMyUsername, gMyLocalIPStr, dest_ip_net, dest_port_mac);
    if (sendErr != noErr) {
        log_debug("Error sending sync discovery response: %d to IP 0x%lX:%u", sendErr, (unsigned long)dest_ip_net, dest_port_mac);
    } else {
        char tempIPStr[INET_ADDRSTRLEN];
        if (gNetworkOps && gNetworkOps->AddressToString) {
            gNetworkOps->AddressToString(dest_ip_net, tempIPStr);
        } else {
            sprintf(tempIPStr, "%lu.%lu.%lu.%lu",
                    (dest_ip_net >> 24) & 0xFF, (dest_ip_net >> 16) & 0xFF,
                    (dest_ip_net >> 8) & 0xFF, dest_ip_net & 0xFF);
        }
        log_debug("Sent DISCOVERY_RESPONSE to %s:%u", tempIPStr, dest_port_mac);
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

OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum)
{
    OSErr err;

    log_debug("Initializing UDP Discovery Endpoint using network abstraction...");

    if (!gNetworkOps) {
        log_app_event("Error: Network abstraction not initialized");
        return notOpenErr;
    }

    if (macTCPRefNum == 0) {
        log_debug("Error (InitUDP): macTCPRefNum is 0.");
        return paramErr;
    }

    /* Clean up any previous state */
    gUDPEndpoint = NULL;
    gUDPRecvBuffer = NULL;
    gUDPReadPending = false;
    gUDPBfrReturnPending = false;
    gLastBroadcastTimeTicks = 0;

    /* Allocate receive buffer */
    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);
    if (gUDPRecvBuffer == NULL) {
        log_app_event("Fatal Error: Could not allocate UDP receive buffer (%ld bytes).", (long)kMinUDPBufSize);
        return memFullErr;
    }
    log_debug("Allocated %ld bytes for UDP receive buffer at 0x%lX.", (long)kMinUDPBufSize, (unsigned long)gUDPRecvBuffer);

    /* Create UDP endpoint using abstraction */
    err = gNetworkOps->UDPCreate(macTCPRefNum, &gUDPEndpoint, PORT_UDP, gUDPRecvBuffer, kMinUDPBufSize);
    if (err != noErr || gUDPEndpoint == NULL) {
        log_app_event("Error (InitUDP): UDPCreate failed (Error: %d).", err);
        DisposePtr(gUDPRecvBuffer);
        gUDPRecvBuffer = NULL;
        return err;
    }

    log_debug("UDP Endpoint created successfully using network abstraction on port %u.", PORT_UDP);

    /* Start initial async read */
    err = StartAsyncUDPRead();
    if (err != noErr && err != 1) {
        log_app_event("Error (InitUDP): Failed to start initial async UDP read. Error: %d", err);
        CleanupUDPDiscoveryEndpoint(macTCPRefNum);
        return err;
    }

    log_debug("Initial asynchronous UDP read started.");
    return noErr;
}

void CleanupUDPDiscoveryEndpoint(short macTCPRefNum)
{
    OSErr err;

    log_debug("Cleaning up UDP Discovery Endpoint...");

    if (gUDPEndpoint != NULL && gNetworkOps && gNetworkOps->UDPRelease) {
        log_debug("Releasing UDP endpoint...");
        err = gNetworkOps->UDPRelease(macTCPRefNum, gUDPEndpoint);
        if (err != noErr) {
            log_debug("Warning: UDPRelease failed during cleanup (Error: %d).", err);
        }
        gUDPEndpoint = NULL;
    }

    gUDPReadPending = false;
    gUDPBfrReturnPending = false;

    if (gUDPRecvBuffer != NULL) {
        log_debug("Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
        DisposePtr(gUDPRecvBuffer);
        gUDPRecvBuffer = NULL;
    }

    gLastBroadcastTimeTicks = 0;
    log_debug("UDP Discovery Endpoint cleanup finished.");
}

OSErr SendDiscoveryBroadcastSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr)
{
    OSErr err;
    int formatted_len;

    (void)macTCPRefNum; /* Not needed with abstraction */

    if (!gNetworkOps || gUDPEndpoint == NULL) return notOpenErr;
    if (myUsername == NULL || myLocalIPStr == NULL) return paramErr;

    log_debug("Sending Discovery Broadcast...");

    /* Format the message */
    formatted_len = format_message(gBroadcastBuffer, BUFFER_SIZE, MSG_DISCOVERY,
                                   myUsername, myLocalIPStr, "");
    if (formatted_len <= 0) {
        log_debug("Error: format_message failed for DISCOVERY");
        return paramErr;
    }

    /* Send using abstraction layer */
    err = gNetworkOps->UDPSend(gUDPEndpoint, BROADCAST_IP, PORT_UDP,
                               (Ptr)gBroadcastBuffer, formatted_len - 1);

    if (err != noErr) {
        log_debug("Error sending broadcast: %d", err);
    } else {
        log_debug("Broadcast sent successfully");
    }

    return err;
}

OSErr SendDiscoveryResponseSync(short macTCPRefNum, const char *myUsername,
                                const char *myLocalIPStr, ip_addr destIP, udp_port destPort)
{
    OSErr err;
    int formatted_len;

    (void)macTCPRefNum; /* Not needed with abstraction */

    if (!gNetworkOps || gUDPEndpoint == NULL) return notOpenErr;
    if (myUsername == NULL || myLocalIPStr == NULL) return paramErr;

    log_debug("Sending Discovery Response to IP 0x%lX:%u...", (unsigned long)destIP, destPort);

    /* Format the message */
    formatted_len = format_message(gResponseBuffer, BUFFER_SIZE, MSG_DISCOVERY_RESPONSE,
                                   myUsername, myLocalIPStr, "");
    if (formatted_len <= 0) {
        log_debug("Error: format_message failed for DISCOVERY_RESPONSE");
        return paramErr;
    }

    /* Send using abstraction layer */
    err = gNetworkOps->UDPSend(gUDPEndpoint, destIP, destPort,
                               (Ptr)gResponseBuffer, formatted_len - 1);

    if (err != noErr) {
        log_debug("Error sending response: %d", err);
    }

    return err;
}

OSErr StartAsyncUDPRead(void)
{
    OSErr err;
    unsigned short bufferSize = kMinUDPBufSize;
    MacTCPUDPEndpoint *endpoint;

    if (!gNetworkOps) return notOpenErr;
    if (gUDPEndpoint == NULL) return invalidStreamPtr;

    /* Get the actual endpoint structure */
    endpoint = (MacTCPUDPEndpoint *)gUDPEndpoint;
    if (!endpoint->isCreated || endpoint->stream == NULL) {
        log_debug("StartAsyncUDPRead: Endpoint not properly initialized");
        return invalidStreamPtr;
    }

    if (gUDPReadPending) {
        log_debug("StartAsyncUDPRead: UDPRead already pending. Ignoring request.");
        return 1;
    }

    if (gUDPBfrReturnPending) {
        log_debug("StartAsyncUDPRead: Cannot start new read, buffer return is pending. Try later.");
        return 1;
    }

    if (gUDPRecvBuffer == NULL) {
        log_debug("Error (StartAsyncUDPRead): gUDPRecvBuffer is NULL.");
        return invalidBufPtr;
    }

    /* For async operations, use the actual stream pointer from the endpoint */
    memset(&gUDPReadPB, 0, sizeof(UDPiopb));
    gUDPReadPB.ioCompletion = nil;
    gUDPReadPB.ioCRefNum = gMacTCPRefNum;
    gUDPReadPB.csCode = UDPRead;
    gUDPReadPB.udpStream = endpoint->stream;  /* Use the actual stream */
    gUDPReadPB.csParam.receive.rcvBuff = gUDPRecvBuffer;
    gUDPReadPB.csParam.receive.rcvBuffLen = kMinUDPBufSize;
    gUDPReadPB.csParam.receive.timeOut = 0;
    gUDPReadPB.ioResult = 1;

    err = PBControlAsync((ParmBlkPtr)&gUDPReadPB);
    if (err != noErr) {
        log_debug("Error (StartAsyncUDPRead): PBControlAsync(UDPRead - polling) failed to LAUNCH. Error: %d", err);
        return err;
    }

    gUDPReadPending = true;
    log_debug("StartAsyncUDPRead: Async UDPRead initiated for polling.");
    return noErr;
}

OSErr ReturnUDPBufferAsync(Ptr dataPtr, unsigned short bufferSize)
{
    OSErr err;
    MacTCPUDPEndpoint *endpoint;

    if (!gNetworkOps || gUDPEndpoint == NULL) return invalidStreamPtr;

    /* Get the actual endpoint structure */
    endpoint = (MacTCPUDPEndpoint *)gUDPEndpoint;

    if (gUDPBfrReturnPending) {
        log_debug("ReturnUDPBufferAsync: Buffer return already pending. Ignoring request.");
        return 1;
    }

    if (dataPtr == NULL) {
        log_debug("Error (ReturnUDPBufferAsync): dataPtr is NULL. Cannot return.");
        return invalidBufPtr;
    }

    /* For async buffer return, use the actual stream pointer */
    memset(&gUDPBfrReturnPB, 0, sizeof(UDPiopb));
    gUDPBfrReturnPB.ioCompletion = nil;
    gUDPBfrReturnPB.ioCRefNum = gMacTCPRefNum;
    gUDPBfrReturnPB.csCode = UDPBfrReturn;
    gUDPBfrReturnPB.udpStream = endpoint->stream;  /* Use the actual stream */
    gUDPBfrReturnPB.csParam.receive.rcvBuff = dataPtr;
    gUDPBfrReturnPB.csParam.receive.rcvBuffLen = bufferSize;
    gUDPBfrReturnPB.ioResult = 1;

    err = PBControlAsync((ParmBlkPtr)&gUDPBfrReturnPB);
    if (err != noErr) {
        log_debug("CRITICAL Error (ReturnUDPBufferAsync): PBControlAsync(UDPBfrReturn - polling) failed to LAUNCH. Error: %d.", err);
        return err;
    }

    gUDPBfrReturnPending = true;
    log_debug("ReturnUDPBufferAsync: Async UDPBfrReturn initiated for buffer 0x%lX.", (unsigned long)dataPtr);
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
        log_debug("CheckSendBroadcast: Interval elapsed. Sending broadcast.");
        OSErr sendErr = SendDiscoveryBroadcastSync(macTCPRefNum, myUsername, myLocalIPStr);
        if (sendErr == noErr) {
            gLastBroadcastTimeTicks = currentTimeTicks;
        } else {
            log_debug("Sync broadcast initiation FAILED (Error: %d). Will retry next interval.", sendErr);
        }
    }
}

void PollUDPListener(short macTCPRefNum, ip_addr myLocalIP)
{
    OSErr ioResult;
    static discovery_platform_callbacks_t mac_callbacks = {
        .send_response_callback = mac_send_discovery_response,
        .add_or_update_peer_callback = mac_add_or_update_peer,
        .notify_peer_list_updated_callback = mac_notify_peer_list_updated
    };

    (void)macTCPRefNum; /* Not needed with abstraction */

    if (!gNetworkOps || gUDPEndpoint == NULL) return;

    /* For now, we still need to check the MacTCP PB directly for async polling */
    /* TODO: Add async polling support to abstraction layer */
    if (gUDPReadPending) {
        ioResult = gUDPReadPB.ioResult;
        if (ioResult <= 0) {
            gUDPReadPending = false;

            if (ioResult == noErr) {
                ip_addr senderIPNet = gUDPReadPB.csParam.receive.remoteHost;
                udp_port senderPortHost = gUDPReadPB.csParam.receive.remotePort;
                unsigned short dataLength = gUDPReadPB.csParam.receive.rcvBuffLen;
                Ptr dataPtr = gUDPReadPB.csParam.receive.rcvBuff;

                if (dataLength > 0) {
                    if (senderIPNet != myLocalIP) {
                        char senderIPStr[INET_ADDRSTRLEN];
                        if (gNetworkOps->AddressToString) {
                            OSErr addrErr = gNetworkOps->AddressToString(senderIPNet, senderIPStr);
                            if (addrErr != noErr) {
                                sprintf(senderIPStr, "%lu.%lu.%lu.%lu",
                                        (senderIPNet >> 24) & 0xFF, (senderIPNet >> 16) & 0xFF,
                                        (senderIPNet >> 8) & 0xFF, senderIPNet & 0xFF);
                            }
                        } else {
                            sprintf(senderIPStr, "%lu.%lu.%lu.%lu",
                                    (senderIPNet >> 24) & 0xFF, (senderIPNet >> 16) & 0xFF,
                                    (senderIPNet >> 8) & 0xFF, senderIPNet & 0xFF);
                        }

                        uint32_t sender_ip_for_shared = (uint32_t)senderIPNet;
                        discovery_logic_process_packet((const char *)dataPtr, dataLength,
                                                       senderIPStr, sender_ip_for_shared, senderPortHost,
                                                       &mac_callbacks, NULL);
                    } else {
                        char selfIPStr[INET_ADDRSTRLEN];
                        if (gNetworkOps->AddressToString) {
                            gNetworkOps->AddressToString(senderIPNet, selfIPStr);
                        } else {
                            sprintf(selfIPStr, "%lu.%lu.%lu.%lu",
                                    (senderIPNet >> 24) & 0xFF, (senderIPNet >> 16) & 0xFF,
                                    (senderIPNet >> 8) & 0xFF, senderIPNet & 0xFF);
                        }
                        log_debug("PollUDPListener: Ignored UDP packet from self (%s).", selfIPStr);
                    }

                    OSErr returnErr = ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize);
                    if (returnErr != noErr && returnErr != 1) {
                        log_debug("CRITICAL Error: Failed to initiate async UDPBfrReturn. Error: %d", returnErr);
                    } else {
                        log_debug("PollUDPListener: Initiated return for buffer 0x%lX.", (unsigned long)dataPtr);
                    }
                } else {
                    log_debug("DEBUG: Async UDPRead returned noErr but 0 bytes. Returning buffer.");
                    (void)ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize);
                }
            } else {
                log_debug("Error (PollUDPListener): Polled async UDPRead completed with error: %d", ioResult);
                if (gUDPReadPB.csParam.receive.rcvBuff) {
                    (void)ReturnUDPBufferAsync(gUDPReadPB.csParam.receive.rcvBuff, kMinUDPBufSize);
                }
            }
        }
    }

    /* Check buffer return completion */
    if (gUDPBfrReturnPending) {
        ioResult = gUDPBfrReturnPB.ioResult;
        if (ioResult <= 0) {
            gUDPBfrReturnPending = false;
            if (ioResult != noErr) {
                log_debug("CRITICAL Error: Polled async UDPBfrReturn completed with error: %d.", ioResult);
            } else {
                log_debug("PollUDPListener: Async UDPBfrReturn completed successfully.");
                if (!gUDPReadPending && gUDPEndpoint != NULL) {
                    StartAsyncUDPRead();
                }
            }
        }
    }

    /* Ensure we always have a read pending */
    if (!gUDPReadPending && !gUDPBfrReturnPending && gUDPEndpoint != NULL) {
        OSErr startErr = StartAsyncUDPRead();
        if (startErr != noErr && startErr != 1) {
            log_debug("PollUDPListener: Failed to start new UDP read in idle fallback. Error: %d", startErr);
        }
    }
}