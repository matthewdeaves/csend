//====================================
// FILE: ./classic_mac/discovery.c
//====================================

#include "discovery.h"
#include "logging.h"
#include "protocol.h"
#include "../shared/discovery_logic.h"
#include "peer_mac.h"
#include "network.h" // For gMacTCPRefNum, gMyUsername, gMyLocalIPStr, AddrToStr
#include "dialog.h"  // For gMainWindow
#include "dialog_peerlist.h" // For gPeerListHandle, UpdatePeerDisplayList
#include <Devices.h>
#include <Errors.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Memory.h>
#include <stddef.h> // For offsetof if needed, though not directly used
#include <MacTCP.h>
// AddrToStr is declared in network.h, but if it were from DNR.c directly:
// extern OSErr AddrToStr(unsigned long addr, char *addrStr);


StreamPtr gUDPStream = NULL;
Ptr gUDPRecvBuffer = NULL;
UDPiopb gUDPReadPB;          // Parameter block for async UDPRead (polling)
UDPiopb gUDPBfrReturnPB;     // Parameter block for async UDPBfrReturn (polling)
Boolean gUDPReadPending = false;
Boolean gUDPBfrReturnPending = false;
unsigned long gLastBroadcastTimeTicks = 0; // For periodic discovery

// Static buffers for sync send operations to avoid heap fragmentation in tight loops
static char gBroadcastBuffer[BUFFER_SIZE];
static struct wdsEntry gBroadcastWDS[2];

static char gResponseBuffer[BUFFER_SIZE];
static struct wdsEntry gResponseWDS[2];


// --- Platform-specific Callbacks for Shared Discovery Logic ---
static void mac_send_discovery_response(uint32_t dest_ip_addr_host, uint16_t dest_port_host, void* platform_context) {
    (void)platform_context; // Unused
    ip_addr dest_ip_net = (ip_addr)dest_ip_addr_host; // MacTCP uses network byte order for ip_addr
    udp_port dest_port_mac = dest_port_host;         // MacTCP uses host byte order for udp_port

    OSErr sendErr = SendDiscoveryResponseSync(gMacTCPRefNum, gMyUsername, gMyLocalIPStr, dest_ip_net, dest_port_mac);
    if (sendErr != noErr) {
        log_message("Error sending sync discovery response: %d", sendErr);
    } else {
         char tempIPStr[INET_ADDRSTRLEN];
         AddrToStr(dest_ip_net, tempIPStr); // Convert network order IP for logging
         log_to_file_only("Sent DISCOVERY_RESPONSE to %s:%u", tempIPStr, dest_port_mac);
    }
}

static int mac_add_or_update_peer(const char* ip, const char* username, void* platform_context) {
    (void)platform_context; // Unused
    return AddOrUpdatePeer(ip, username);
}

static void mac_notify_peer_list_updated(void* platform_context) {
    (void)platform_context; // Unused
    if (gMainWindow != NULL && gPeerListHandle != NULL) {
        UpdatePeerDisplayList(true); // Force redraw
    }
}


OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum) {
    OSErr err;
    UDPiopb pbCreate;
    const unsigned short specificPort = PORT_UDP; // Host byte order

    log_message("Initializing UDP Discovery Endpoint (Async Read Poll / Sync Write)...");

    if (macTCPRefNum == 0) {
        log_message("Error (InitUDP): macTCPRefNum is 0.");
        return paramErr;
    }

    // Initialize globals
    gUDPStream = NULL;
    gUDPRecvBuffer = NULL;
    gUDPReadPending = false;
    gUDPBfrReturnPending = false;
    gLastBroadcastTimeTicks = 0; // Initialize for CheckSendBroadcast

    // Allocate receive buffer
    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);
    if (gUDPRecvBuffer == NULL) {
        log_message("Fatal Error: Could not allocate UDP receive buffer (%ld bytes).", (long)kMinUDPBufSize);
        return memFullErr;
    }
    log_message("Allocated %ld bytes for UDP receive buffer at 0x%lX.", (long)kMinUDPBufSize, (unsigned long)gUDPRecvBuffer);

    // Prepare UDPCreate parameter block
    memset(&pbCreate, 0, sizeof(UDPiopb));
    pbCreate.ioCompletion = nil; // Synchronous call
    pbCreate.ioCRefNum = macTCPRefNum;
    pbCreate.csCode = UDPCreate;
    pbCreate.udpStream = 0L; // Will be filled by MacTCP
    pbCreate.csParam.create.rcvBuff = gUDPRecvBuffer;
    pbCreate.csParam.create.rcvBuffLen = kMinUDPBufSize;
    pbCreate.csParam.create.notifyProc = nil; // No ASR for this stream
    pbCreate.csParam.create.localPort = specificPort; // Request specific port

    log_message("Calling PBControlSync (UDPCreate) for port %u...", specificPort);
    err = PBControlSync((ParmBlkPtr)&pbCreate);

    // Capture results before potential early exit
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
        return ioErr; // Treat as an IO error
    }
     if (assignedPort != specificPort && specificPort != 0) { // Only warn if a specific non-zero port was requested
         log_message("Warning (InitUDP): UDPCreate assigned port %u instead of requested %u.", assignedPort, specificPort);
     }

    gUDPStream = returnedStreamPtr;
    log_message("UDP Endpoint created successfully (StreamPtr: 0x%lX) on assigned port %u.", (unsigned long)gUDPStream, assignedPort);

    // Reset pending flags (should be redundant if Init is called once, but good practice)
    gUDPReadPending = false;
    gUDPBfrReturnPending = false;
    gLastBroadcastTimeTicks = 0; // Reset for CheckSendBroadcast

    // Start the first asynchronous read (for polling)
    err = StartAsyncUDPRead();
    if (err != noErr && err != 1 /*pending*/) { // Check for immediate error from StartAsyncUDPRead
        log_message("Error (InitUDP): Failed to start initial async UDP read (polling). Error: %d", err);
        CleanupUDPDiscoveryEndpoint(macTCPRefNum); // Full cleanup on error
        return err;
    } else {
        // If err == 1 (pending), it's fine. If err == noErr (completed sync, unlikely for async setup), also fine.
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

        // *** PBKillIO calls REMOVED based on Q&A PLAT04 ***
        // UDPRelease is documented to terminate outstanding commands.
        // if (gUDPReadPending) { PBKillIO((ParmBlkPtr)&gUDPReadPB, false); } // REMOVED
        // if (gUDPBfrReturnPending) { PBKillIO((ParmBlkPtr)&gUDPBfrReturnPB, false); } // REMOVED

        // UDPRelease will terminate pending UDPRead/UDPBfrReturn.
        // Set flags to false to prevent PollUDPListener from acting on old PBs after release.
        gUDPReadPending = false;
        gUDPBfrReturnPending = false;

        memset(&pbRelease, 0, sizeof(UDPiopb));
        pbRelease.ioCompletion = nil; // Synchronous call
        pbRelease.ioCRefNum = macTCPRefNum;
        pbRelease.csCode = UDPRelease;
        pbRelease.udpStream = gUDPStream;
        // The rcvBuff and rcvBuffLen in csParam.create for UDPRelease are ignored by MacTCP
        // but some docs show them being filled; clear for safety.
        pbRelease.csParam.create.rcvBuff = NULL;
        pbRelease.csParam.create.rcvBuffLen = 0;

        err = PBControlSync((ParmBlkPtr)&pbRelease);
        if (err != noErr) {
            log_message("Warning: Synchronous UDPRelease failed during cleanup (Error: %d).", err);
        } else {
            log_message("Synchronous UDPRelease succeeded.");
        }
        gUDPStream = NULL; // Mark stream as closed
    } else {
        log_message("UDP Stream was not open or already cleaned up.");
    }

    if (gUDPRecvBuffer != NULL) {
         log_message("Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
         DisposePtr(gUDPRecvBuffer);
         gUDPRecvBuffer = NULL;
    }

    // Ensure flags are reset even if stream was already NULL
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
        return 1; // Still pending, not an error
    }
    if (gUDPBfrReturnPending) {
        // This is a valid state; we might be waiting for a buffer to be returned
        // before we can issue a new read. PollUDPListener should handle this.
        log_to_file_only("StartAsyncUDPRead: Cannot start read, buffer return is pending.");
        return 1; // Indicate an operation is in progress that prevents new read
    }
    if (gUDPRecvBuffer == NULL) { // Should have been allocated in Init
        log_message("Error (StartAsyncUDPRead): gUDPRecvBuffer is NULL.");
        return invalidBufPtr;
    }

    memset(&gUDPReadPB, 0, sizeof(UDPiopb));
    gUDPReadPB.ioCompletion = nil; // We are polling ioResult
    gUDPReadPB.ioCRefNum = gMacTCPRefNum;
    gUDPReadPB.csCode = UDPRead;
    gUDPReadPB.udpStream = gUDPStream;
    gUDPReadPB.csParam.receive.rcvBuff = gUDPRecvBuffer;
    gUDPReadPB.csParam.receive.rcvBuffLen = kMinUDPBufSize;
    gUDPReadPB.csParam.receive.timeOut = 0; // Infinite timeout for async read (we poll)
    // remoteHost and remotePort are filled in by MacTCP on receive

    gUDPReadPending = true; // Set before calling async
    gUDPReadPB.ioResult = 1; // Mark as pending for polling logic

    err = PBControlAsync((ParmBlkPtr)&gUDPReadPB);
    if (err != noErr) {
        log_message("Error (StartAsyncUDPRead): PBControlAsync(UDPRead - polling) failed immediately. Error: %d", err);
        gUDPReadPending = false; // Reset flag on immediate failure
        return err;
    }

    log_to_file_only("StartAsyncUDPRead: Async UDPRead initiated for polling.");
    return 1; // Indicates operation is pending
}

// Internal helper for sending UDP packets synchronously
static OSErr SendUDPSyncInternal(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr,
                           const char *msgType, const char *content,
                           ip_addr destIP, udp_port destPort,
                           char *staticBuffer, struct wdsEntry *staticWDS)
{
    OSErr err;
    int formatted_len;
    UDPiopb pbSync;

    if (gUDPStream == NULL) return invalidStreamPtr; // Check if UDP endpoint is initialized
    if (macTCPRefNum == 0) return paramErr;
    if (myUsername == NULL || myLocalIPStr == NULL) return paramErr; // Basic validation

    // Format the message into the provided static buffer
    formatted_len = format_message(staticBuffer, BUFFER_SIZE, msgType, myUsername, myLocalIPStr, content);
    if (formatted_len <= 0) {
        log_message("Error (SendUDPSyncInternal): format_message failed for '%s'.", msgType);
        return paramErr; // Or a more specific error
    }

    // Prepare WDS (Write Data Structure)
    // format_message includes the null terminator in its length. UDP sends binary data.
    // Send length should be actual data length, not including C-string null terminator.
    staticWDS[0].length = formatted_len -1; // Exclude null terminator for UDP payload
    staticWDS[0].ptr = staticBuffer;
    staticWDS[1].length = 0; // End of WDS list
    staticWDS[1].ptr = nil;

    // Prepare UDPWrite parameter block
    memset(&pbSync, 0, sizeof(UDPiopb));
    pbSync.ioCompletion = nil; // Synchronous call
    pbSync.ioCRefNum = macTCPRefNum;
    pbSync.csCode = UDPWrite;
    pbSync.udpStream = gUDPStream;
    pbSync.csParam.send.remoteHost = destIP;
    pbSync.csParam.send.remotePort = destPort;
    pbSync.csParam.send.wdsPtr = (Ptr)staticWDS;
    pbSync.csParam.send.checkSum = true; // Let MacTCP calculate checksum
    pbSync.csParam.send.sendLength = 0;  // Ignored when WDS is used

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
                                MSG_DISCOVERY, "", // Empty content for discovery
                                BROADCAST_IP, PORT_UDP,
                                gBroadcastBuffer, gBroadcastWDS);
}

OSErr SendDiscoveryResponseSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr, ip_addr destIP, udp_port destPort) {
     log_to_file_only("Sending Discovery Response to IP %lu:%u...", (unsigned long)destIP, destPort);
     return SendUDPSyncInternal(macTCPRefNum, myUsername, myLocalIPStr,
                                MSG_DISCOVERY_RESPONSE, "", // Empty content for response
                                destIP, destPort,
                                gResponseBuffer, gResponseWDS);
}


OSErr ReturnUDPBufferAsync(Ptr dataPtr, unsigned short bufferSize) {
    OSErr err;

    if (gUDPStream == NULL) return invalidStreamPtr;
    if (gUDPBfrReturnPending) {
         log_to_file_only("ReturnUDPBufferAsync: Buffer return already pending.");
         return 1; // Still pending
    }
    if (dataPtr == NULL) { // Should be gUDPRecvBuffer
        log_message("Error (ReturnUDPBufferAsync): dataPtr is NULL.");
        return invalidBufPtr;
    }

    memset(&gUDPBfrReturnPB, 0, sizeof(UDPiopb));
    gUDPBfrReturnPB.ioCompletion = nil; // We are polling ioResult
    gUDPBfrReturnPB.ioCRefNum = gMacTCPRefNum;
    gUDPBfrReturnPB.csCode = UDPBfrReturn;
    gUDPBfrReturnPB.udpStream = gUDPStream;
    gUDPBfrReturnPB.csParam.receive.rcvBuff = dataPtr; // The buffer to return
    gUDPBfrReturnPB.csParam.receive.rcvBuffLen = bufferSize; // Its original size

    gUDPBfrReturnPending = true; // Set before calling async
    gUDPBfrReturnPB.ioResult = 1; // Mark as pending for polling logic

    err = PBControlAsync((ParmBlkPtr)&gUDPBfrReturnPB);
    if (err != noErr) {
        log_message("CRITICAL Error (ReturnUDPBufferAsync): PBControlAsync(UDPBfrReturn - polling) failed immediately. Error: %d.", err);
        gUDPBfrReturnPending = false; // Reset flag on immediate failure
        return err;
    }

    log_to_file_only("ReturnUDPBufferAsync: Async UDPBfrReturn initiated for buffer 0x%lX.", (unsigned long)dataPtr);
    return 1; // Indicates operation is pending
}


void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr) {
    unsigned long currentTimeTicks = TickCount();
    // DISCOVERY_INTERVAL is in seconds, TickCount is 1/60th sec
    const unsigned long intervalTicks = (unsigned long)DISCOVERY_INTERVAL * 60UL;

    if (gUDPStream == NULL || macTCPRefNum == 0) return; // Not initialized

    // Handle TickCount wrap-around for gLastBroadcastTimeTicks
    if (currentTimeTicks < gLastBroadcastTimeTicks) { // Timer wrapped
        gLastBroadcastTimeTicks = currentTimeTicks; // Reset base to current time
    }

    if (gLastBroadcastTimeTicks == 0 || (currentTimeTicks - gLastBroadcastTimeTicks) >= intervalTicks) {
        log_to_file_only("CheckSendBroadcast: Interval elapsed. Sending broadcast.");
        OSErr sendErr = SendDiscoveryBroadcastSync(macTCPRefNum, myUsername, myLocalIPStr);
        if (sendErr == noErr) {
            gLastBroadcastTimeTicks = currentTimeTicks; // Update time only on successful send
        } else {
            log_message("Sync broadcast initiation failed (Error: %d)", sendErr);
            // Consider if gLastBroadcastTimeTicks should be updated even on failure to prevent rapid retries
            // For now, only update on success.
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

    // Check for completed UDPRead
    if (gUDPReadPending) {
        ioResult = gUDPReadPB.ioResult; // Check result from async call
        if (ioResult <= 0) { // Operation completed (successfully or with error)
            gUDPReadPending = false; // Clear flag

            if (ioResult == noErr) {
                ip_addr senderIPNet = gUDPReadPB.csParam.receive.remoteHost;
                udp_port senderPortHost = gUDPReadPB.csParam.receive.remotePort;
                unsigned short dataLength = gUDPReadPB.csParam.receive.rcvBuffLen;
                Ptr dataPtr = gUDPReadPB.csParam.receive.rcvBuff; // This is gUDPRecvBuffer

                if (dataLength > 0) {
                    if (senderIPNet != myLocalIP) { // Don't process our own broadcasts
                        char senderIPStr[INET_ADDRSTRLEN];
                        OSErr addrErr = AddrToStr(senderIPNet, senderIPStr);
                        if (addrErr != noErr) {
                            // Fallback IP string formatting if AddrToStr fails
                            sprintf(senderIPStr, "%lu.%lu.%lu.%lu", (senderIPNet >> 24) & 0xFF, (senderIPNet >> 16) & 0xFF, (senderIPNet >> 8) & 0xFF, senderIPNet & 0xFF);
                            log_to_file_only("PollUDPListener: AddrToStr failed (%d) for sender IP %lu. Using fallback '%s'.", addrErr, (unsigned long)senderIPNet, senderIPStr);
                        }

                        // Convert sender_ip_net (network order) to host order for shared logic if it expects host order
                        // However, discovery_logic_process_packet takes sender_ip_addr which is uint32_t,
                        // and for MacTCP, ip_addr is already in network byte order.
                        // The shared logic should be aware of this or convert if necessary.
                        // For now, assume shared logic can handle network order IP or uses the string.
                        uint32_t sender_ip_addr_host_order_for_shared = (uint32_t)senderIPNet; // Pass as is

                        discovery_logic_process_packet((const char*)dataPtr, dataLength,
                                                       senderIPStr, sender_ip_addr_host_order_for_shared, senderPortHost,
                                                       &mac_callbacks,
                                                       NULL); // No specific platform context needed for these callbacks
                    } else {
                        // Log ignored self-packet
                        char selfIPStr[INET_ADDRSTRLEN];
                        AddrToStr(senderIPNet, selfIPStr); // Should be gMyLocalIPStr
                        log_to_file_only("PollUDPListener: Ignored UDP packet from self (%s).", selfIPStr);
                    }

                    // Return the buffer
                    OSErr returnErr = ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize);
                    if (returnErr != noErr && returnErr != 1 /*pending*/) {
                        log_message("CRITICAL Error (PollUDPListener): Failed to initiate async UDPBfrReturn (polling) using pointer 0x%lX after processing. Error: %d.", (unsigned long)dataPtr, returnErr);
                        // What to do here? Maybe try to StartAsyncUDPRead again later.
                    } else {
                         log_to_file_only("PollUDPListener: Initiated return for buffer 0x%lX.", (unsigned long)dataPtr);
                    }
                } else { // dataLength == 0
                    log_to_file_only("DEBUG: Async UDPRead (polling) returned 0 bytes.");
                    ReturnUDPBufferAsync(dataPtr, kMinUDPBufSize); // Still need to return the buffer
                }
            } else { // ioResult < 0 (error)
                log_message("Error (PollUDPListener): Polled async UDPRead completed with error: %d", ioResult);
                // Still need to return the buffer even on error
                ReturnUDPBufferAsync(gUDPReadPB.csParam.receive.rcvBuff, kMinUDPBufSize);
            }
        }
        // If ioResult > 0, it's still pending. Do nothing.
    }

    // Check for completed UDPBfrReturn
    if (gUDPBfrReturnPending) {
        ioResult = gUDPBfrReturnPB.ioResult; // Check result from async call
        if (ioResult <= 0) { // Operation completed
            gUDPBfrReturnPending = false; // Clear flag

            if (ioResult != noErr) {
                log_message("CRITICAL Error (PollUDPListener): Polled async UDPBfrReturn completed with error: %d.", ioResult);
                // If buffer return fails, we might be in trouble for future reads.
            } else {
                 log_to_file_only("PollUDPListener: Async UDPBfrReturn completed successfully.");
                 // Now that buffer is returned, if a read isn't already pending, start one.
                 if (!gUDPReadPending) {
                     StartAsyncUDPRead();
                 }
            }
        }
        // If ioResult > 0, it's still pending. Do nothing.
    }

    // Fallback: If nothing is pending and stream is open, ensure a read is started.
    // This handles cases where initial StartAsyncUDPRead might have failed and needs retry,
    // or if both read and return completed in the same poll cycle.
    if (!gUDPReadPending && !gUDPBfrReturnPending && gUDPStream != NULL) {
        log_to_file_only("PollUDPListener: No UDP read or buffer return pending, starting new read.");
        StartAsyncUDPRead();
    }
}