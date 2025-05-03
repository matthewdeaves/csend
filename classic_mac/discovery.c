//====================================
// FILE: ./classic_mac/discovery.c
//====================================

#include "discovery.h"
#include "logging.h"
#include "protocol.h"
#include "peer_mac.h"
#include "network.h"
#include "dialog.h" // Added for gMyUsername
#include <Devices.h>
#include <Errors.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Memory.h>
#include <stddef.h>

StreamPtr gUDPStream = NULL;
Ptr gUDPRecvBuffer = NULL;
unsigned long gLastBroadcastTimeTicks = 0;

// Function to print layout info (for debugging, can be removed later)
void PrintUDPiopbLayout() {
    log_message("--- Layout Log (Using standard UDPiopb for offsets in this test) ---");
    log_message("sizeof(UDPiopb) (from <MacTCP.h>) = %lu", (unsigned long)sizeof(UDPiopb));
    log_message("Offset ioCompletion (UDPiopb) = %lu", (unsigned long)offsetof(UDPiopb, ioCompletion));
    log_message("Offset ioResult (UDPiopb) = %lu", (unsigned long)offsetof(UDPiopb, ioResult));
    log_message("Offset ioCRefNum (UDPiopb) = %lu", (unsigned long)offsetof(UDPiopb, ioCRefNum));
    log_message("Offset csCode (UDPiopb) = %lu", (unsigned long)offsetof(UDPiopb, csCode));
    log_message("Offset udpStream (UDPiopb) = %lu", (unsigned long)offsetof(UDPiopb, udpStream));
    log_message("Offset csParam (UDPiopb) = %lu", (unsigned long)offsetof(UDPiopb, csParam));
    log_message("  --- csParam.create (UDPiopb) ---");
    log_message("  sizeof(UDPCreatePB) = %lu", (unsigned long)sizeof(struct UDPCreatePB));
    log_message("  Offset create.rcvBuff = %lu", (unsigned long)offsetof(UDPiopb, csParam.create.rcvBuff));
    log_message("  Offset create.rcvBuffLen = %lu", (unsigned long)offsetof(UDPiopb, csParam.create.rcvBuffLen));
    log_message("  Offset create.notifyProc = %lu", (unsigned long)offsetof(UDPiopb, csParam.create.notifyProc));
    log_message("  Offset create.localPort = %lu", (unsigned long)offsetof(UDPiopb, csParam.create.localPort));
    log_message("  Offset create.userDataPtr = %lu", (unsigned long)offsetof(UDPiopb, csParam.create.userDataPtr));
    log_message("  --- csParam.receive (UDPiopb) ---");
    log_message("  sizeof(UDPReceivePB) = %lu", (unsigned long)sizeof(struct UDPReceivePB));
    log_message("  Offset receive.timeout = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.timeOut));
    log_message("  Offset receive.remoteHost = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.remoteHost));
    log_message("  Offset receive.remotePort = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.remotePort));
    log_message("  Offset receive.rcvBuff = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.rcvBuff));
    log_message("  Offset receive.rcvBuffLen = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.rcvBuffLen));
    log_message("  Offset receive.secondTimeStamp = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.secondTimeStamp));
    log_message("  Offset receive.userDataPtr = %lu", (unsigned long)offsetof(UDPiopb, csParam.receive.userDataPtr));
    log_message("--- End Layout ---");
}

OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum) {
    OSErr err;
    UDPiopb pbCreate;
    const unsigned short specificPort = PORT_UDP;

    log_message("Initializing UDP Discovery Endpoint...");
    log_message("Using standard UDPiopb (expected 58 bytes) for ALL calls.");

    if (macTCPRefNum == 0) {
         log_message("Error (InitUDP): Invalid MacTCP RefNum: %d", macTCPRefNum);
         return paramErr;
    }

    // Allocate buffer for receiving UDP packets
    gUDPRecvBuffer = NewPtrClear(kMinUDPBufSize);
    if (gUDPRecvBuffer == NULL) {
        log_message("Error (InitUDP): Failed to allocate UDP receive buffer (memFullErr).");
        return memFullErr;
    }
    log_message("Allocated %ld bytes for UDP receive buffer at 0x%lX.", (long)kMinUDPBufSize, (unsigned long)gUDPRecvBuffer);

    // Prepare the parameter block for UDPCreate
    memset(&pbCreate, 0, sizeof(UDPiopb));
    pbCreate.ioCompletion = nil;             // Synchronous call
    pbCreate.ioCRefNum = macTCPRefNum;       // MacTCP driver reference number
    pbCreate.csCode = udpCreate;             // Command code for creating UDP endpoint
    pbCreate.udpStream = 0L;                 // Output: Stream pointer will be returned here
    pbCreate.csParam.create.rcvBuff = gUDPRecvBuffer; // Pointer to our receive buffer
    pbCreate.csParam.create.rcvBuffLen = kMinUDPBufSize; // Size of the receive buffer
    pbCreate.csParam.create.notifyProc = nil; // No asynchronous notification routine
    pbCreate.csParam.create.localPort = specificPort; // Request specific UDP port

    log_message("Calling PBControlSync (udpCreate) using UDPiopb (expected 58 bytes) for port %u...", specificPort);
    err = PBControlSync((ParmBlkPtr)&pbCreate);
    log_message("DEBUG: After PBControlSync(udpCreate) using UDPiopb: err=%d, pbCreate.udpStream=0x%lX, pbCreate.csParam.create.localPort=%u",
        err, (unsigned long)pbCreate.udpStream, pbCreate.csParam.create.localPort);

    if (err != noErr) {
        log_message("Error (InitUDP): PBControlSync(udpCreate) failed. Error: %d", err);
        if (gUDPRecvBuffer != NULL) { DisposePtr(gUDPRecvBuffer); gUDPRecvBuffer = NULL; }
        gUDPStream = NULL;
        return err;
    }

    // Store the returned stream pointer
    gUDPStream = pbCreate.udpStream;
    log_message("DEBUG: Stored StreamPtr directly from pbCreate.udpStream: 0x%lX", (unsigned long)gUDPStream);

    // Sanity checks
    if (gUDPStream == NULL) {
         log_message("CRITICAL WARNING (InitUDP): StreamPtr is NULL. UDP will likely fail.");
         if (gUDPRecvBuffer != NULL) { DisposePtr(gUDPRecvBuffer); gUDPRecvBuffer = NULL; }
         gUDPStream = NULL;
         return ioErr; // Indicate a failure
    }
    if (gUDPStream == (StreamPtr)gUDPRecvBuffer) {
        // This shouldn't happen with a successful udpCreate
        log_message("WARNING (InitUDP): StreamPtr (0x%lX) is the same as gUDPRecvBuffer (0x%lX). This might be incorrect.",
                    (unsigned long)gUDPStream, (unsigned long)gUDPRecvBuffer);
    }

    unsigned short assignedPort = pbCreate.csParam.create.localPort;
    log_message("UDP Endpoint created successfully (StreamPtr: 0x%lX) on assigned port %u.", (unsigned long)gUDPStream, assignedPort);

    gLastBroadcastTimeTicks = 0; // Initialize broadcast timer
    return noErr;
}

OSErr SendDiscoveryBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr) {
    OSErr err;
    char buffer[BUFFER_SIZE];     // Buffer for the formatted message
    struct wdsEntry wds[2];       // Write Data Structure for MacTCP
    UDPiopb pb;                   // Parameter block for the UDPWrite call
    int formatted_len;

    if (gUDPStream == NULL) {
        log_message("Error (SendUDP): Cannot send broadcast, UDP stream pointer is NULL.");
        return invalidStreamPtr;
    }
    if (macTCPRefNum == 0) {
         log_message("Error (SendUDP): Invalid MacTCP RefNum: %d", macTCPRefNum);
         return paramErr;
     }
    if (myUsername == NULL || myLocalIPStr == NULL) {
         log_message("Error (SendUDP): Missing username or IP string.");
         return paramErr;
     }

    // Format the discovery message
    formatted_len = format_message(buffer, BUFFER_SIZE, MSG_DISCOVERY, myUsername, myLocalIPStr, "");
    if (formatted_len <= 0) {
        log_message("Error (SendUDP): Failed to format discovery broadcast message.");
        return paramErr; // Or a more specific error
    }

    // Set up the Write Data Structure (WDS)
    // We send formatted_len - 1 because format_message includes the null terminator
    wds[0].length = formatted_len - 1;
    wds[0].ptr = buffer;
    wds[1].length = 0; // Terminator for the WDS list
    wds[1].ptr = nil;

    // Prepare the parameter block for UDPWrite
    memset(&pb, 0, sizeof(UDPiopb));
    pb.ioCompletion = nil;             // Synchronous call
    pb.ioCRefNum = macTCPRefNum;       // MacTCP driver reference number
    pb.csCode = udpWrite;              // Command code for writing UDP data
    pb.udpStream = gUDPStream;         // Use the stream pointer we obtained earlier
    pb.csParam.send.remoteHost = BROADCAST_IP; // Target IP address (broadcast)
    pb.csParam.send.remotePort = PORT_UDP;     // Target UDP port
    pb.csParam.send.wdsPtr = (Ptr)wds; // Pointer to our WDS
    pb.csParam.send.checkSum = true;   // Request MacTCP to calculate checksum

    // Make the synchronous call to send the UDP packet
    err = PBControlSync((ParmBlkPtr)&pb);

    if (err != noErr) {
        log_message("Error (SendUDP): PBControlSync(udpWrite) failed with stream 0x%lX using UDPiopb. Error: %d", (unsigned long)gUDPStream, err);
        return err;
    }

    // Update the last broadcast time if successful
    gLastBroadcastTimeTicks = TickCount();
    // log_message("Discovery broadcast sent successfully."); // Optional: Can be verbose
    return noErr;
}

void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr) {
    unsigned long currentTimeTicks = TickCount();
    const unsigned long intervalTicks = (unsigned long)DISCOVERY_INTERVAL * 60; // Convert seconds to ticks

    if (gUDPStream == NULL || macTCPRefNum == 0) return; // Cannot send without a stream/refnum

    // Handle TickCount wrap-around (though unlikely to matter for 10-second intervals)
    if (currentTimeTicks < gLastBroadcastTimeTicks) {
        gLastBroadcastTimeTicks = currentTimeTicks; // Reset timer if wrap-around detected
    }

    // Send broadcast if it's the first time or the interval has passed
    if (gLastBroadcastTimeTicks == 0 || (currentTimeTicks - gLastBroadcastTimeTicks) >= intervalTicks) {
        OSErr sendErr = SendDiscoveryBroadcast(macTCPRefNum, myUsername, myLocalIPStr);
        if (sendErr != noErr) {
            log_message("Periodic broadcast failed (Error: %d)", sendErr);
        }
        // gLastBroadcastTimeTicks is updated inside SendDiscoveryBroadcast on success
    }
}

void CheckUDPReceive(short macTCPRefNum, ip_addr myLocalIP) {
    OSErr err;
    UDPiopb pbRead;               // Parameter block for udpRead
    char senderIPStrFromHeader[INET_ADDRSTRLEN];
    char senderIPStrFromPayload[INET_ADDRSTRLEN];
    char senderUsername[32];
    char msgType[32];
    char content[BUFFER_SIZE];
    unsigned short bytesReceived = 0;
    Ptr receivedDataPtr = NULL;
    ip_addr senderIP = 0;
    udp_port senderPort = 0;      // Store sender's port

    // Basic checks before attempting to read
    if (gUDPStream == NULL || macTCPRefNum == 0 || gUDPRecvBuffer == NULL) {
        return;
    }

    // Prepare parameter block for udpRead
    memset(&pbRead, 0, sizeof(UDPiopb));
    pbRead.ioCompletion = nil;             // Synchronous call
    pbRead.ioCRefNum = macTCPRefNum;       // MacTCP driver ref num
    pbRead.csCode = udpRead;               // Command code for reading UDP
    pbRead.udpStream = gUDPStream;         // Our UDP stream pointer
    // --- Fields specific to udpRead ---
    pbRead.csParam.receive.rcvBuff = gUDPRecvBuffer; // Buffer to store received data
    pbRead.csParam.receive.rcvBuffLen = kMinUDPBufSize; // Max bytes to read into buffer
    pbRead.csParam.receive.timeOut = 1;    // Short timeout (in seconds) - effectively non-blocking
    pbRead.csParam.receive.secondTimeStamp = 0; // Not used here
    // --- Output fields for udpRead ---
    pbRead.csParam.receive.remoteHost = 0; // Will be filled with sender's IP
    pbRead.csParam.receive.remotePort = 0; // Will be filled with sender's port

    // Attempt to read a UDP packet
    err = PBControlSync((ParmBlkPtr)&pbRead);

    if (err == noErr) {
        // Successfully received a packet
        senderIP = pbRead.csParam.receive.remoteHost;
        senderPort = pbRead.csParam.receive.remotePort; // Get the sender's port
        bytesReceived = pbRead.csParam.receive.rcvBuffLen; // Actual bytes received
        receivedDataPtr = pbRead.csParam.receive.rcvBuff; // Pointer to the data within our buffer

        if (bytesReceived > 0) {
            log_message("DEBUG: udpRead returned: bytes=%u, receivedDataPtr=0x%lX, senderIP=%lu, senderPort=%u",
                        bytesReceived, (unsigned long)receivedDataPtr, senderIP, senderPort);

            // Ignore packets from ourselves
            if (senderIP != myLocalIP) {
                 // Convert sender IP to string for logging/use
                 OSErr addrErr = AddrToStr(senderIP, senderIPStrFromHeader);
                 if (addrErr != noErr) {
                     log_message("Warning: AddrToStr failed for sender IP %lu (Error: %d). Using raw IP.", senderIP, addrErr);
                     // Fallback to manual formatting if AddrToStr fails
                     sprintf(senderIPStrFromHeader, "%lu.%lu.%lu.%lu", (senderIP >> 24) & 0xFF, (senderIP >> 16) & 0xFF, (senderIP >> 8) & 0xFF, senderIP & 0xFF);
                 }

                 // Parse the received data using our protocol function
                 if (parse_message(receivedDataPtr, bytesReceived, senderIPStrFromPayload, senderUsername, msgType, content) == 0) {
                    // Successfully parsed the message
                    if (strcmp(msgType, MSG_DISCOVERY_RESPONSE) == 0) {
                        log_message("Received DISCOVERY_RESPONSE from %s@%s", senderUsername, senderIPStrFromHeader);
                        // Update our peer list
                        if (AddOrUpdatePeer(senderIPStrFromHeader, senderUsername) < 0) {
                            log_message("Peer list full, could not add %s@%s", senderUsername, senderIPStrFromHeader);
                        }
                    } else if (strcmp(msgType, MSG_DISCOVERY) == 0) {
                         log_message("Received DISCOVERY from %s@%s", senderUsername, senderIPStrFromHeader);

                         // *** SEND RESPONSE ***
                         UDPiopb pbSend;
                         char responseBuffer[BUFFER_SIZE];
                         struct wdsEntry wds[2];
                         int formatted_len;
                         OSErr sendErr;

                         // Format the response message
                         formatted_len = format_message(responseBuffer, BUFFER_SIZE, MSG_DISCOVERY_RESPONSE,
                                                        gMyUsername, gMyLocalIPStr, "");

                         if (formatted_len > 0) {
                             // Set up WDS for the response
                             wds[0].length = formatted_len - 1; // Exclude null terminator
                             wds[0].ptr = responseBuffer;
                             wds[1].length = 0;
                             wds[1].ptr = nil;

                             // Prepare parameter block for sending the response
                             memset(&pbSend, 0, sizeof(UDPiopb));
                             pbSend.ioCompletion = nil;
                             pbSend.ioCRefNum = macTCPRefNum;
                             pbSend.csCode = udpWrite;
                             pbSend.udpStream = gUDPStream;
                             pbSend.csParam.send.remoteHost = senderIP; // Send back to original sender IP
                             pbSend.csParam.send.remotePort = senderPort; // Send back to original sender port
                             pbSend.csParam.send.wdsPtr = (Ptr)wds;
                             pbSend.csParam.send.checkSum = true;

                             // Send the response
                             sendErr = PBControlSync((ParmBlkPtr)&pbSend);
                             if (sendErr != noErr) {
                                 log_message("Error sending DISCOVERY_RESPONSE to %s. Error: %d", senderIPStrFromHeader, sendErr);
                             } else {
                                 log_message("Sent DISCOVERY_RESPONSE to %s@%s", senderUsername, senderIPStrFromHeader);
                             }
                         } else {
                             log_message("Error formatting DISCOVERY_RESPONSE.");
                         }
                         // *** END SEND RESPONSE ***

                         // Also update peer list based on discovery message
                         if (AddOrUpdatePeer(senderIPStrFromHeader, senderUsername) < 0) {
                             log_message("Peer list full, could not add %s@%s", senderUsername, senderIPStrFromHeader);
                         }
                    }
                    // Add handling for other message types (like MSG_TEXT via UDP if needed) here
                 } else {
                     // Failed to parse the message according to our protocol
                     log_message("Discarding invalid/unknown UDP msg from %s (%u bytes).",
                                 senderIPStrFromHeader, bytesReceived);
                     // Optional: Log raw data for debugging
                     if (gLogFile != NULL) {
                        fprintf(gLogFile, "    Raw Data (%u bytes): [", bytesReceived);
                        for (unsigned short i = 0; i < bytesReceived && i < kMinUDPBufSize; ++i) {
                             unsigned char c = (unsigned char)receivedDataPtr[i];
                             if (c >= 32 && c <= 126) { fputc(c, gLogFile); }
                             else { fprintf(gLogFile, "\\x%02X", c); }
                        }
                        fprintf(gLogFile, "]\n");
                        fflush(gLogFile);
                     }
                 }
            } else {
                // log_message("DEBUG: Ignored UDP packet from self (%s).", senderIPStrFromHeader);
            }

            // *** IMPORTANT: Return the buffer to MacTCP ***
            // This must be done after processing *any* successfully received packet
            // to allow MacTCP to reuse the buffer space.
            UDPiopb bfrReturnPB;
            OSErr returnErr;
            memset(&bfrReturnPB, 0, sizeof(UDPiopb));
            bfrReturnPB.ioCompletion = nil;
            bfrReturnPB.ioCRefNum = macTCPRefNum;
            bfrReturnPB.csCode = udpBfrReturn;
            bfrReturnPB.udpStream = gUDPStream;
            // Pass back the *exact* pointer and original buffer size MacTCP expects
            bfrReturnPB.csParam.receive.rcvBuff = receivedDataPtr; // Pointer from udpRead result
            bfrReturnPB.csParam.receive.rcvBuffLen = kMinUDPBufSize; // Original size allocated

            log_message("DEBUG: Before udpBfrReturn: gUDPStream=0x%lX, Passing rcvBuff=0x%lX (from udpRead result), rcvBuffLen=%u",
                        (unsigned long)gUDPStream,
                        (unsigned long)bfrReturnPB.csParam.receive.rcvBuff,
                        bfrReturnPB.csParam.receive.rcvBuffLen);

            returnErr = PBControlSync((ParmBlkPtr)&bfrReturnPB);
            if (returnErr != noErr) {
                // This is serious, MacTCP might run out of buffers if this fails repeatedly
                log_message("CRITICAL Error (UDP Receive): PBControlSync(udpBfrReturn) failed using UDPiopb. Error: %d.", returnErr);
            } else {
                 log_message("DEBUG: PBControlSync(udpBfrReturn) using UDPiopb succeeded.");
            }
            // *** Buffer returned ***
        } else {
             // udpRead returned noErr but 0 bytes. This is possible but unusual.
             log_message("DEBUG: udpRead returned 0 bytes.");
             // Still need to return the buffer even if 0 bytes were read?
             // MacTCP docs suggest returning buffer only for non-zero data reads.
             // Let's stick to that unless problems arise.
        }
    } else if (err == commandTimeout) {
        // No packet received within the timeout period. This is normal.
    } else {
        // An actual error occurred during udpRead
        log_message("Error (UDP Receive): PBControlSync(udpRead) failed using UDPiopb. Error: %d", err);
        // Consider if any cleanup is needed here, though usually just logging is sufficient.
    }
}

void CleanupUDPDiscoveryEndpoint(short macTCPRefNum) {
    UDPiopb pb;
    OSErr err;

    log_message("Cleaning up UDP Discovery Endpoint...");

    if (gUDPStream != NULL) {
        if (macTCPRefNum == 0) {
             log_message("Warning (CleanupUDP): Invalid MacTCP RefNum (%d), cannot release UDP stream.", macTCPRefNum);
        } else {
            log_message("Attempting PBControlSync (udpRelease) using UDPiopb (58 bytes) for endpoint 0x%lX...", (unsigned long)gUDPStream);
            memset(&pb, 0, sizeof(UDPiopb));
            pb.ioCompletion = nil;
            pb.ioCRefNum = macTCPRefNum;
            pb.csCode = udpRelease;
            pb.udpStream = gUDPStream;

            // Provide the original buffer info back to udpRelease
            // Although docs are ambiguous, providing it seems safer.
            if (gUDPRecvBuffer != NULL) {
                pb.csParam.create.rcvBuff = gUDPRecvBuffer;
                pb.csParam.create.rcvBuffLen = kMinUDPBufSize; // Pass the original size

                err = PBControlSync((ParmBlkPtr)&pb);
                if (err != noErr) {
                    log_message("Warning (CleanupUDP): PBControlSync(udpRelease) failed using UDPiopb. Error: %d", err);
                    // Even if release fails, we should still try to dispose the buffer.
                } else {
                    log_message("PBControlSync(udpRelease) using UDPiopb succeeded.");
                }
            } else {
                 log_message("Warning (CleanupUDP): Cannot call udpRelease because receive buffer pointer is NULL (already disposed?).");
            }
        }
    } else {
        log_message("UDP Endpoint was not open or already invalid, skipping release.");
    }

    // Mark stream as invalid regardless of release success/failure
    gUDPStream = NULL;

    // Dispose the buffer if it hasn't been already
    if (gUDPRecvBuffer != NULL) {
         log_message("Disposing UDP receive buffer at 0x%lX.", (unsigned long)gUDPRecvBuffer);
         DisposePtr(gUDPRecvBuffer);
         gUDPRecvBuffer = NULL;
    }

    log_message("UDP Discovery Endpoint cleanup finished.");
}
