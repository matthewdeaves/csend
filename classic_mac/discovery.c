#include "discovery.h"
#include "logging.h"
#include "protocol.h"
#include "../shared/discovery_logic.h"
#include "peer_mac.h"
#include "network.h"
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
static void mac_send_discovery_response(uint32_t dest_ip_addr_host, uint16_t dest_port_host, void* platform_context) {
    (void)platform_context;
    ip_addr dest_ip_net = (ip_addr)dest_ip_addr_host;
    udp_port dest_port_mac = dest_port_host;
    OSErr sendErr = SendDiscoveryResponseSync(gMacTCPRefNum, gMyUsername, gMyLocalIPStr, dest_ip_net, dest_port_mac);
    if (sendErr != noErr) {
        log_message("Error sending sync discovery response: %d", sendErr);
    } else {
         char tempIPStr[INET_ADDRSTRLEN];
         AddrToStr(dest_ip_net, tempIPStr);
         log_to_file_only("Sent DISCOVERY_RESPONSE to %s:%u", tempIPStr, dest_port_mac);
    }
}
static int mac_add_or_update_peer(const char* ip, const char* username, void* platform_context) {
    (void)platform_context;
    return AddOrUpdatePeer(ip, username);
}
static void mac_notify_peer_list_updated(void* platform_context) {
    (void)platform_context;
    if (gMainWindow != NULL && gPeerListHandle != NULL) {
        UpdatePeerDisplayList(true);
    }
}
OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum) {
    OSErr err;
    UDPiopb pbCreate;
    const unsigned short specificPort = PORT_UDP;
    log_message("Initializing UDP Discovery Endpoint (Async Read Poll / Sync Write)...");
    if (macTCPRefNum == 0) {
        log_message("Error (InitUDP): macTCPRefNum is 0.");
        return paramErr;
    }
    gUDPStream = NULL;
    gUDPRecvBuffer = NULL;
    gUDPReadPending = false;
    gUDPBfrReturnPending = false;
    gLastBroadcastTimeTicks = 0;
    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);
    if (gUDPRecvBuffer == NULL) {
        log_message("Fatal Error: Could not allocate UDP receive buffer (%ld bytes).", (long)kMinUDPBufSize);
        return memFullErr;
    }
    log_message("Allocated %ld bytes for UDP receive buffer at 0x%lX.", (long)kMinUDPBufSize, (unsigned long)gUDPRecvBuffer);
    memset(&pbCreate, 0, sizeof(UDPiopb));
    pbCreate.ioCompletion = nil;
    pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = UDPCreate;
    pbCreate.udpStream = 0L;
    pbCreate.csParam.create.rcvBuff = gUDPRecvBuffer;
    pbCreate.csParam.create.rcvBuffLen = kMinUDPBufSize;
    pbCreate.csParam.create.notifyProc = nil;
    pbCreate.csParam.create.localPort = specificPort;
    log_message("Calling PBControlSync (UDPCreate) for port %u...", specificPort);
    err = PBControlSync((ParmBlkPtr)&pbCreate);
    StreamPtr returnedStreamPtr = pbCreate.udpStream;
    unsigned short assignedPort = pbCreate.csParam.create.localPort;
    log_message("DEBUG: After PBControlSync(UDPCreate): err=%d, StreamPtr=0x%lX, AssignedPort=%u",
        err, (unsigned long)returnedStreamPtr, assignedPort);
    if (err != noErr) {
        log_message("Error (InitUDP): UDPCreate failed (Error: %d).", err);
        DisposePtr(gUDPRecvBuffer); gUDPRecvBuffer = NULL;
        return err;
    }
    if (returnedStreamPtr == NULL) {
        log_message("Error (InitUDP): UDPCreate succeeded but returned NULL stream pointer.");
        DisposePtr(gUDPRecvBuffer); gUDPRecvBuffer = NULL;
        return ioErr;
    }
     if (assignedPort != specificPort && specificPort != 0) {
         log_message("Warning (InitUDP): UDPCreate assigned port %u instead of requested %u.", assignedPort, specificPort);
     }
    gUDPStream = returnedStreamPtr;
    log_message("UDP Endpoint created successfully (StreamPtr: 0x%lX) on assigned port %u.", (unsigned long)gUDPStream, assignedPort);
    gUDPReadPending = false;
    gUDPBfrReturnPending = false;
    gLastBroadcastTimeTicks = 0;
    err = StartAsyncUDPRead();
    if (err != noErr && err != 1 ) {
        log_message("Error (InitUDP): Failed to start initial async UDP read (polling). Error: %d", err);
        CleanupUDPDiscoveryEndpoint(macTCPRefNum);
        return err;
    } else {
        log_message("Initial asynchronous UDP read (polling) STARTING.");
    }
    return noErr;
}
void CleanupUDPDiscoveryEndpoint(short macTCPRefNum) {
    UDPiopb pbRelease;
    OSErr err;
    log_message("Cleaning up UDP Discovery Endpoint (Async)...");
    if (gUDPStream != NULL) {
        log_message("UDP Stream 0x%lX was open. Attempting synchronous release...", (unsigned long)gUDPStream);
        gUDPReadPending = false;
        gUDPBfrReturnPending = false;
        memset(&pbRelease, 0, sizeof(UDPiopb));
        pbRelease.ioCompletion = nil;
        pbRelease.ioCRefNum = macTCPRefNum;
        pbRelease.csCode = UDPRelease;
        pbRelease.udpStream = gUDPStream;
        pbRelease.csParam.create.rcvBuff = NULL;
        pbRelease.csParam.create.rcvBuffLen = 0;
        err = PBControlSync((ParmBlkPtr)&pbRelease);
        if (err != noErr) {
            log_message("Warning: Synchronous UDPRelease failed during cleanup (Error: %d).", err);
        } else {
            log_message("Synchronous UDPRelease succeeded.");
        }
        gUDPStream = NULL;
    } else {
        log_message("UDP Stream was not open or already cleaned up.");
    }
    if (gUDPRecvBuffer != NULL) {
         log_message("Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
         DisposePtr(gUDPRecvBuffer);
         gUDPRecvBuffer = NULL;
    }
    gUDPReadPending = false;
    gUDPBfrReturnPending = false;
    gLastBroadcastTimeTicks = 0;
    log_message("UDP Discovery Endpoint cleanup finished.");
}
OSErr StartAsyncUDPRead(void) {
    OSErr err;
    if (gUDPStream == NULL) return invalidStreamPtr;
    if (gUDPReadPending) {
        log_to_file_only("StartAsyncUDPRead: UDPRead already pending.");
        return 1;
    }
    if (gUDPBfrReturnPending) {
        log_to_file_only("StartAsyncUDPRead: Cannot start read, buffer return is pending.");
        return 1;
    }
    if (gUDPRecvBuffer == NULL) {
        log_message("Error (StartAsyncUDPRead): gUDPRecvBuffer is NULL.");
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
    gUDPReadPending = true;
    gUDPReadPB.ioResult = 1;
    err = PBControlAsync((ParmBlkPtr)&gUDPReadPB);
    if (err != noErr) {
        log_message("Error (StartAsyncUDPRead): PBControlAsync(UDPRead - polling) failed immediately. Error: %d", err);
        gUDPReadPending = false;
        return err;
    }
    log_to_file_only("StartAsyncUDPRead: Async UDPRead initiated for polling.");
    return 1;
}
static OSErr SendUDPSyncInternal(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr,
                           const char *msgType, const char *content,
                           ip_addr destIP, udp_port destPort,
                           char *staticBuffer, struct wdsEntry *staticWDS)
{
    OSErr err;
    int formatted_len;
    UDPiopb pbSync;
    if (gUDPStream == NULL) return invalidStreamPtr;
    if (macTCPRefNum == 0) return paramErr;
    if (myUsername == NULL || myLocalIPStr == NULL) return paramErr;
    formatted_len = format_message(staticBuffer, BUFFER_SIZE, msgType, myUsername, myLocalIPStr, content);
    if (formatted_len <= 0) {
        log_message("Error (SendUDPSyncInternal): format_message failed for '%s'.", msgType);
        return paramErr;
    }
    staticWDS[0].length = formatted_len -1;
    staticWDS[0].ptr = staticBuffer;
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
        log_message("Error (SendUDPSync): PBControlSync(UDPWrite) for '%s' failed. Error: %d", msgType, err);
        return err;
    }
    log_to_file_only("SendUDPSyncInternal: Sent '%s' to IP %lu:%u.", msgType, (unsigned long)destIP, destPort);
    return noErr;
}
OSErr SendDiscoveryBroadcastSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr) {
    log_to_file_only("Sending Discovery Broadcast...");
    return SendUDPSyncInternal(macTCPRefNum, myUsername, myLocalIPStr,
                                MSG_DISCOVERY, "",
                                BROADCAST_IP, PORT_UDP,
                                gBroadcastBuffer, gBroadcastWDS);
}
OSErr SendDiscoveryResponseSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr, ip_addr destIP, udp_port destPort) {
     log_to_file_only("Sending Discovery Response to IP %lu:%u...", (unsigned long)destIP, destPort);
     return SendUDPSyncInternal(macTCPRefNum, myUsername, myLocalIPStr,
                                MSG_DISCOVERY_RESPONSE, "",
                                destIP, destPort,
                                gResponseBuffer, gResponseWDS);
}
OSErr ReturnUDPBufferAsync(Ptr dataPtr, unsigned short bufferSize) {
    OSErr err;
    if (gUDPStream == NULL) return invalidStreamPtr;
    if (gUDPBfrReturnPending) {
         log_to_file_only("ReturnUDPBufferAsync: Buffer return already pending.");
         return 1;
    }
    if (dataPtr == NULL) {
        log_message("Error (ReturnUDPBufferAsync): dataPtr is NULL.");
        return invalidBufPtr;
    }
    memset(&gUDPBfrReturnPB, 0, sizeof(UDPiopb));
    gUDPBfrReturnPB.ioCompletion = nil;
    gUDPBfrReturnPB.ioCRefNum = gMacTCPRefNum;
    gUDPBfrReturnPB.csCode = UDPBfrReturn;
    gUDPBfrReturnPB.udpStream = gUDPStream;
    gUDPBfrReturnPB.csParam.receive.rcvBuff = dataPtr;
    gUDPBfrReturnPB.csParam.receive.rcvBuffLen = bufferSize;
    gUDPBfrReturnPending = true;
    gUDPBfrReturnPB.ioResult = 1;
    err = PBControlAsync((ParmBlkPtr)&gUDPBfrReturnPB);
    if (err != noErr) {
        log_message("CRITICAL Error (ReturnUDPBufferAsync): PBControlAsync(UDPBfrReturn - polling) failed immediately. Error: %d.", err);
        gUDPBfrReturnPending = false;
        return err;
    }
    log_to_file_only("ReturnUDPBufferAsync: Async UDPBfrReturn initiated for buffer 0x%lX.", (unsigned long)dataPtr);
    return 1;
}
void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr) {
    unsigned long currentTimeTicks = TickCount();
    const unsigned long intervalTicks = (unsigned long)DISCOVERY_INTERVAL * 60UL;
    if (gUDPStream == NULL || macTCPRefNum == 0) return;
    if (currentTimeTicks < gLastBroadcastTimeTicks) {
        gLastBroadcastTimeTicks = currentTimeTicks;
    }
    if (gLastBroadcastTimeTicks == 0 || (currentTimeTicks - gLastBroadcastTimeTicks) >= intervalTicks) {
        log_to_file_only("CheckSendBroadcast: Interval elapsed. Sending broadcast.");
        OSErr sendErr = SendDiscoveryBroadcastSync(macTCPRefNum, myUsername, myLocalIPStr);
        if (sendErr == noErr) {
            gLastBroadcastTimeTicks = currentTimeTicks;
        } else {
            log_message("Sync broadcast initiation failed (Error: %d)", sendErr);
        }
    }
}
void PollUDPListener(short macTCPRefNum, ip_addr myLocalIP) {
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
                            log_to_file_only("PollUDPListener: AddrToStr failed (%d) for sender IP %lu. Using fallback '%s'.", addrErr, (unsigned long)senderIPNet, senderIPStr);
                        }
                        uint32_t sender_ip_addr_host_order_for_shared = (uint32_t)senderIPNet;
                        discovery_logic_process_packet((const char*)dataPtr, dataLength,
                                                       senderIPStr, sender_ip_addr_host_order_for_shared, senderPortHost,
                                                       &mac_callbacks,
                                                       NULL);
                    } else {
                        char selfIPStr[INET_ADDRSTRLEN];
                        AddrToStr(senderIPNet, selfIPStr);
                        log_to_file_only("PollUDPListener: Ignored UDP packet from self (%s).", selfIPStr);
                    }
                    OSErr returnErr = ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize);
                    if (returnErr != noErr && returnErr != 1 ) {
                        log_message("CRITICAL Error (PollUDPListener): Failed to initiate async UDPBfrReturn (polling) using pointer 0x%lX after processing. Error: %d.", (unsigned long)dataPtr, returnErr);
                    } else {
                         log_to_file_only("PollUDPListener: Initiated return for buffer 0x%lX.", (unsigned long)dataPtr);
                    }
                } else {
                    log_to_file_only("DEBUG: Async UDPRead (polling) returned 0 bytes.");
                    ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize);
                }
            } else {
                log_message("Error (PollUDPListener): Polled async UDPRead completed with error: %d", ioResult);
                ReturnUDPBufferAsync(gUDPReadPB.csParam.receive.rcvBuff, kMinUDPBufSize);
            }
        }
    }
    if (gUDPBfrReturnPending) {
        ioResult = gUDPBfrReturnPB.ioResult;
        if (ioResult <= 0) {
            gUDPBfrReturnPending = false;
            if (ioResult != noErr) {
                log_message("CRITICAL Error (PollUDPListener): Polled async UDPBfrReturn completed with error: %d.", ioResult);
            } else {
                 log_to_file_only("PollUDPListener: Async UDPBfrReturn completed successfully.");
                 if (!gUDPReadPending) {
                     StartAsyncUDPRead();
                 }
            }
        }
    }
    if (!gUDPReadPending && !gUDPBfrReturnPending && gUDPStream != NULL) {
        log_to_file_only("PollUDPListener: No UDP read or buffer return pending, starting new read.");
        StartAsyncUDPRead();
    }
}
