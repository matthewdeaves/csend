#include "mactcp_discovery.h"
#include "logging.h"
#include "../shared/logging.h"
#include "protocol.h"
#include "../shared/discovery.h"
#include "peer.h"
#include "mactcp_network.h"
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
StreamPtr gUDPStream = NULL;
Ptr gUDPRecvBuffer = NULL;
UDPiopb gUDPReadPB;
UDPiopb gUDPBfrReturnPB;
Boolean gUDPReadPending = false;
Boolean gUDPBfrReturnPending = false;
unsigned long gLastBroadcastTimeTicks = 0;
static char gBroadcastBuffer[BUFFER_SIZE];
static struct wdsEntry gBroadcastWDS[2];
static char gResponseBuffer[BUFFER_SIZE];
static struct wdsEntry gResponseWDS[2];
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
        AddrToStr(dest_ip_net, tempIPStr);
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
    UDPiopb pbCreate;
    const unsigned short specificPort = PORT_UDP;
    log_debug("Initializing UDP Discovery Endpoint (Async Read Poll / Sync Write)...");
    if (macTCPRefNum == 0) {
        log_debug("Error (InitUDP): macTCPRefNum is 0.");
        return paramErr;
    }
    gUDPStream = NULL;
    gUDPRecvBuffer = NULL;
    gUDPReadPending = false;
    gUDPBfrReturnPending = false;
    gLastBroadcastTimeTicks = 0;
    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);
    if (gUDPRecvBuffer == NULL) {
        log_app_event("Fatal Error: Could not allocate UDP receive buffer (%ld bytes).", (long)kMinUDPBufSize);
        return memFullErr;
    }
    log_debug("Allocated %ld bytes for UDP receive buffer at 0x%lX.", (long)kMinUDPBufSize, (unsigned long)gUDPRecvBuffer);
    memset(&pbCreate, 0, sizeof(UDPiopb));
    pbCreate.ioCompletion = nil;
    pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = UDPCreate;
    pbCreate.udpStream = 0L;
    pbCreate.csParam.create.rcvBuff = gUDPRecvBuffer;
    pbCreate.csParam.create.rcvBuffLen = kMinUDPBufSize;
    pbCreate.csParam.create.notifyProc = nil;
    pbCreate.csParam.create.localPort = specificPort;
    log_debug("Calling PBControlSync (UDPCreate) for port %u...", specificPort);
    err = PBControlSync((ParmBlkPtr)&pbCreate);
    StreamPtr returnedStreamPtr = pbCreate.udpStream;
    unsigned short assignedPort = pbCreate.csParam.create.localPort;
    log_debug("DEBUG: After PBControlSync(UDPCreate): err=%d, Returned StreamPtr=0x%lX (Our BufferPtr=0x%lX), AssignedPort=%u",
              err, (unsigned long)returnedStreamPtr, (unsigned long)gUDPRecvBuffer, assignedPort);
    if (err != noErr) {
        log_app_event("Error (InitUDP): UDPCreate failed (Error: %d).", err);
        DisposePtr(gUDPRecvBuffer);
        gUDPRecvBuffer = NULL;
        return err;
    }
    if (returnedStreamPtr == NULL) {
        log_app_event("Error (InitUDP): UDPCreate succeeded but returned NULL stream pointer.");
        DisposePtr(gUDPRecvBuffer);
        gUDPRecvBuffer = NULL;
        return ioErr;
    }
    if (assignedPort != specificPort && specificPort != 0) {
        log_app_event("Warning (InitUDP): UDPCreate assigned port %u instead of requested %u. Discovery may fail.", assignedPort, specificPort);
    }
    gUDPStream = returnedStreamPtr;
    log_debug("UDP Endpoint created successfully (gUDPStream: 0x%lX) on assigned port %u.", (unsigned long)gUDPStream, assignedPort);
    err = StartAsyncUDPRead();
    if (err != noErr && err != 1) {
        log_app_event("Error (InitUDP): Failed to start initial async UDP read (polling). Error: %d", err);
        CleanupUDPDiscoveryEndpoint(macTCPRefNum);
        return err;
    } else if (err == noErr || err == 1) {
        log_debug("Initial asynchronous UDP read (polling) STARTING (err code %d means launched or was already pending).", err);
    }
    return noErr;
}
void CleanupUDPDiscoveryEndpoint(short macTCPRefNum)
{
    UDPiopb pbRelease;
    OSErr err;
    log_debug("Cleaning up UDP Discovery Endpoint...");
    if (gUDPStream != NULL) {
        log_debug("UDP Stream 0x%lX was open. Attempting synchronous UDPRelease...", (unsigned long)gUDPStream);
        memset(&pbRelease, 0, sizeof(UDPiopb));
        pbRelease.ioCompletion = nil;
        pbRelease.ioCRefNum = macTCPRefNum;
        pbRelease.csCode = UDPRelease;
        pbRelease.udpStream = gUDPStream;
        pbRelease.csParam.create.rcvBuff = NULL;
        pbRelease.csParam.create.rcvBuffLen = 0;
        err = PBControlSync((ParmBlkPtr)&pbRelease);
        if (err != noErr) {
            log_debug("Warning: Synchronous UDPRelease FAILED during cleanup (Error: %d) for stream 0x%lX.", err, (unsigned long)gUDPStream);
        } else {
            log_debug("Synchronous UDPRelease succeeded for stream 0x%lX.", (unsigned long)gUDPStream);
        }
        gUDPStream = NULL;
    } else {
        log_debug("UDP Stream was not open or already cleaned up.");
    }
    if (gUDPReadPending) {
        log_debug("Clearing gUDPReadPending flag as UDP stream is released.");
        gUDPReadPending = false;
    }
    if (gUDPBfrReturnPending) {
        log_debug("Clearing gUDPBfrReturnPending flag as UDP stream is released.");
        gUDPBfrReturnPending = false;
    }
    if (gUDPRecvBuffer != NULL) {
        log_debug("Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
        DisposePtr(gUDPRecvBuffer);
        gUDPRecvBuffer = NULL;
    }
    gLastBroadcastTimeTicks = 0;
    log_debug("UDP Discovery Endpoint cleanup finished.");
}
OSErr StartAsyncUDPRead(void)
{
    OSErr err;
    if (gUDPStream == NULL) return invalidStreamPtr;
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
    memset(&gUDPReadPB, 0, sizeof(UDPiopb));
    gUDPReadPB.ioCompletion = nil;
    gUDPReadPB.ioCRefNum = gMacTCPRefNum;
    gUDPReadPB.csCode = UDPRead;
    gUDPReadPB.udpStream = gUDPStream;
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
static OSErr SendUDPSyncInternal(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr,
                                 const char *msgType, const char *content,
                                 ip_addr destIP, udp_port destPort,
                                 char *staticSendBuffer, struct wdsEntry *staticWDS)
{
    OSErr err;
    int formatted_len;
    UDPiopb pbSync;
    if (gUDPStream == NULL) return invalidStreamPtr;
    if (macTCPRefNum == 0) return paramErr;
    if (myUsername == NULL || myLocalIPStr == NULL) return paramErr;
    if (staticSendBuffer == NULL || staticWDS == NULL) return paramErr;
    formatted_len = format_message(staticSendBuffer, BUFFER_SIZE, msgType, myUsername, myLocalIPStr, content);
    if (formatted_len <= 0) {
        log_debug("Error (SendUDPSyncInternal): format_message failed for msgType '%s'. Len: %d", msgType, formatted_len);
        return paramErr;
    }
    staticWDS[0].length = formatted_len - 1;
    staticWDS[0].ptr = staticSendBuffer;
    staticWDS[1].length = 0;
    staticWDS[1].ptr = nil;
    memset(&pbSync, 0, sizeof(UDPiopb));
    pbSync.ioCompletion = nil;
    pbSync.ioCRefNum = macTCPRefNum;
    pbSync.csCode = UDPWrite;
    pbSync.udpStream = gUDPStream;
    pbSync.csParam.send.remoteHost = destIP;
    pbSync.csParam.send.remotePort = destPort;
    pbSync.csParam.send.wdsPtr = (Ptr)staticWDS;
    pbSync.csParam.send.checkSum = true;
    pbSync.csParam.send.sendLength = 0;
    err = PBControlSync((ParmBlkPtr)&pbSync);
    if (err != noErr) {
        log_debug("Error (SendUDPSync): PBControlSync(UDPWrite) for '%s' to IP 0x%lX:%u FAILED. Error: %d", msgType, (unsigned long)destIP, destPort, err);
        return err;
    }
    log_debug("SendUDPSyncInternal: Sent '%s' to IP 0x%lX:%u.", msgType, (unsigned long)destIP, destPort);
    return noErr;
}
OSErr SendDiscoveryBroadcastSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr)
{
    log_debug("Sending Discovery Broadcast...");
    return SendUDPSyncInternal(macTCPRefNum, myUsername, myLocalIPStr,
                               MSG_DISCOVERY, "",
                               BROADCAST_IP, PORT_UDP,
                               gBroadcastBuffer, gBroadcastWDS);
}
OSErr SendDiscoveryResponseSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr, ip_addr destIP, udp_port destPort)
{
    log_debug("Sending Discovery Response to IP 0x%lX:%u...", (unsigned long)destIP, destPort);
    return SendUDPSyncInternal(macTCPRefNum, myUsername, myLocalIPStr,
                               MSG_DISCOVERY_RESPONSE, "",
                               destIP, destPort,
                               gResponseBuffer, gResponseWDS);
}
OSErr ReturnUDPBufferAsync(Ptr dataPtr, unsigned short bufferSize)
{
    OSErr err;
    if (gUDPStream == NULL) return invalidStreamPtr;
    if (gUDPBfrReturnPending) {
        log_debug("ReturnUDPBufferAsync: Buffer return already pending. Ignoring request.");
        return 1;
    }
    if (dataPtr == NULL) {
        log_debug("Error (ReturnUDPBufferAsync): dataPtr is NULL. Cannot return.");
        return invalidBufPtr;
    }
    memset(&gUDPBfrReturnPB, 0, sizeof(UDPiopb));
    gUDPBfrReturnPB.ioCompletion = nil;
    gUDPBfrReturnPB.ioCRefNum = gMacTCPRefNum;
    gUDPBfrReturnPB.csCode = UDPBfrReturn;
    gUDPBfrReturnPB.udpStream = gUDPStream;
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
    if (gUDPStream == NULL || macTCPRefNum == 0) return;
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
                        OSErr addrErr = AddrToStr(senderIPNet, senderIPStr);
                        if (addrErr != noErr) {
                            sprintf(senderIPStr, "%lu.%lu.%lu.%lu", (senderIPNet >> 24) & 0xFF, (senderIPNet >> 16) & 0xFF, (senderIPNet >> 8) & 0xFF, senderIPNet & 0xFF);
                            log_debug("PollUDPListener: AddrToStr failed (%d) for sender IP 0x%lX. Using fallback '%s'.", addrErr, (unsigned long)senderIPNet, senderIPStr);
                        }
                        uint32_t sender_ip_for_shared = (uint32_t)senderIPNet;
                        discovery_logic_process_packet((const char *)dataPtr, dataLength,
                                                       senderIPStr, sender_ip_for_shared, senderPortHost,
                                                       &mac_callbacks,
                                                       NULL);
                    } else {
                        char selfIPStr[INET_ADDRSTRLEN];
                        AddrToStr(senderIPNet, selfIPStr);
                        log_debug("PollUDPListener: Ignored UDP packet from self (%s).", selfIPStr);
                    }
                    OSErr returnErr = ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize);
                    if (returnErr != noErr && returnErr != 1) {
                        log_debug("CRITICAL Error (PollUDPListener): Failed to initiate async UDPBfrReturn (polling) after processing. Error: %d. Buffer: 0x%lX", returnErr, (unsigned long)dataPtr);
                    } else {
                        log_debug("PollUDPListener: Initiated return for buffer 0x%lX.", (unsigned long)dataPtr);
                    }
                } else {
                    log_debug("DEBUG: Async UDPRead (polling) returned noErr but 0 bytes. Returning buffer.");
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
    if (gUDPBfrReturnPending) {
        ioResult = gUDPBfrReturnPB.ioResult;
        if (ioResult <= 0) {
            gUDPBfrReturnPending = false;
            if (ioResult != noErr) {
                log_debug("CRITICAL Error (PollUDPListener): Polled async UDPBfrReturn completed with error: %d.", ioResult);
            } else {
                log_debug("PollUDPListener: Async UDPBfrReturn completed successfully.");
                if (!gUDPReadPending && gUDPStream != NULL) {
                    StartAsyncUDPRead();
                }
            }
        }
    }
    if (!gUDPReadPending && !gUDPBfrReturnPending && gUDPStream != NULL) {
        OSErr startErr = StartAsyncUDPRead();
        if (startErr != noErr && startErr != 1) {
            log_debug("PollUDPListener: Failed to start new UDP read in idle fallback. Error: %d", startErr);
        }
    }
}
