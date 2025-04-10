// FILE: ./classic_mac/network.c
#include "network.h"
#include "logging.h"   // For LogToDialog
#include "dialog.h"    // For gMyUsername
#include "protocol.h"  // For format_message, parse_message, MSG_*

#include <Devices.h>   // For PBControlSync, PBControlAsync, TickCount, CntrlParam, ParmBlkPtr
#include <string.h>    // For memset, strlen, strcmp, strcpy
#include <stdlib.h>    // For NULL
#include <Memory.h>    // For BlockMoveData, NewPtr, DisposePtr
#include <MixedMode.h> // For UPP routines

// --- Global Variable Definitions ---
short   gMacTCPRefNum = 0;
ip_addr gMyLocalIP = 0;
char    gMyLocalIPStr[INET_ADDRSTRLEN] = "0.0.0.0";
unsigned long gLastBroadcastTimeTicks = 0;

// Peer List Globals
peer_t  gPeerList[MAX_PEERS];
short   gPeerCount = 0;

// UDP Read Globals
UDPiopb gUDPReadPB;
char    gUDPRecvBuffer[BUFFER_SIZE]; // Used for parsing output, not direct receive
Boolean gUDPReadPending = false;
Ptr     gUDPReceiveAreaPtr = NULL;
UDPIOCompletionUPP gUDPReadCompletionUPP = NULL;

// --- ADDED: State for Deferred Processing ---
Boolean gNeedToSendResponse = false;
ip_addr gResponseDestIP = 0;
udp_port gResponseDestPort = 0;

// --- Static Global for UDP Endpoint ---
static StreamPtr sUDPEndpoint = NULL;

// --- Initialization and Cleanup ---
// ... (InitializeNetworking remains the same) ...
OSErr InitializeNetworking(void) {
    OSErr err;
    ParamBlockRec pb; // PBOpen uses standard ParamBlockRec
    CntrlParam cntrlPB; // ipctlGetAddr uses CntrlParam

    LogToDialog("Initializing Networking...");

    // --- Open MacTCP Driver ---
    pb.ioParam.ioNamePtr = (StringPtr)kTCPDriverName;
    pb.ioParam.ioPermssn = fsCurPerm;
    LogToDialog("Attempting PBOpenSync for .IPP driver...");
    err = PBOpenSync(&pb);
    if (err != noErr) {
        LogToDialog("Error: PBOpenSync failed. Error: %d", err);
        gMacTCPRefNum = 0;
        return err;
    }
    gMacTCPRefNum = pb.ioParam.ioRefNum;
    LogToDialog("PBOpenSync succeeded (RefNum: %d).", gMacTCPRefNum);

    // --- Get Local IP Address ---
    memset(&cntrlPB, 0, sizeof(CntrlParam));
    cntrlPB.ioCRefNum = gMacTCPRefNum;
    cntrlPB.csCode = ipctlGetAddr;
    LogToDialog("Attempting PBControlSync for ipctlGetAddr...");
    err = PBControlSync((ParmBlkPtr)&cntrlPB);
    if (err != noErr) {
        LogToDialog("Error: PBControlSync(ipctlGetAddr) failed. Error: %d", err);
        gMacTCPRefNum = 0; // Mark driver as unusable
        return err;
    }
    LogToDialog("PBControlSync(ipctlGetAddr) succeeded.");
    // Correctly extract the IP address (it's a long starting at csParam[0])
    gMyLocalIP = *((ip_addr *)(&cntrlPB.csParam[0]));

    // --- Initialize DNR FIRST ---
    LogToDialog("Attempting OpenResolver...");
    err = OpenResolver(NULL);
    if (err != noErr) {
        LogToDialog("Error: OpenResolver failed. Error: %d", err);
        gMacTCPRefNum = 0; // Mark driver as unusable
        return err; // Return the OpenResolver error
    } else {
        LogToDialog("OpenResolver succeeded.");
    }

    // --- Convert Local IP to String AFTER DNR is open ---
    LogToDialog("Attempting AddrToStr for IP: %lu...", gMyLocalIP);
    // AddrToStr returns OSErr, but we treat it as informational for now
    AddrToStr(gMyLocalIP, gMyLocalIPStr);
    // Check if the result is valid
    if (strcmp(gMyLocalIPStr, "0.0.0.0") == 0 || gMyLocalIPStr[0] == '\0') {
         LogToDialog("Warning: AddrToStr failed to get a valid IP string. Result: '%s'", gMyLocalIPStr);
         // Consider setting a default or error state if IP is critical
         // strcpy(gMyLocalIPStr, "?.?.?.?"); // Indicate unknown IP
    } else {
        LogToDialog("AddrToStr finished. Result string: '%s'", gMyLocalIPStr);
    }

    // --- Initialize Peer List ---
    InitPeerList();

    LogToDialog("Networking initialization complete.");
    return noErr; // Return noErr if we got this far
}

// ... (CleanupNetworking remains the same) ...
void CleanupNetworking(void) {
    UDPiopb udpPB; // Use UDPiopb for udpRelease
    OSErr err;

    LogToDialog("Cleaning up Networking...");

    // --- Close DNR ---
    LogToDialog("Attempting CloseResolver...");
    err = CloseResolver();
    if (err != noErr) {
        LogToDialog("Warning: CloseResolver failed. Error: %d", err);
    } else {
        LogToDialog("CloseResolver succeeded.");
    }

    // --- Release UDP Endpoint ---
    if (sUDPEndpoint != NULL) {
        LogToDialog("Attempting PBControlSync (udpRelease) for endpoint 0x%lX...", (unsigned long)sUDPEndpoint);
        memset(&udpPB, 0, sizeof(UDPiopb)); // Use UDPiopb
        udpPB.ioCRefNum = gMacTCPRefNum;
        udpPB.csCode = udpRelease;
        udpPB.udpStream = sUDPEndpoint; // Set the stream pointer directly
        // No other input parameters needed for udpRelease

        err = PBControlSync((ParmBlkPtr)&udpPB);
        if (err != noErr) {
            LogToDialog("Warning: PBControlSync(udpRelease) failed. Error: %d", err);
        } else {
            LogToDialog("PBControlSync(udpRelease) succeeded.");
            // Output parameters (rcvBuff, rcvBuffLen) are in udpPB.csParam.create if needed
        }
        sUDPEndpoint = NULL;
    } else {
        LogToDialog("UDP Endpoint was not open, skipping release.");
    }

    // --- Dispose Dynamically Allocated UDP Receive Buffer ---
    if (gUDPReceiveAreaPtr != NULL) {
        LogToDialog("Disposing UDP receive buffer...");
        DisposePtr(gUDPReceiveAreaPtr);
        gUDPReceiveAreaPtr = NULL;
    }

    // --- Dispose UDP Completion Routine UPP --- ADDED
    if (gUDPReadCompletionUPP != NULL) {
        LogToDialog("Disposing UDP completion UPP...");
        DisposeRoutineDescriptor(gUDPReadCompletionUPP);
        gUDPReadCompletionUPP = NULL;
    }

    // --- Close MacTCP Driver ---
    // DO NOT CALL PBCloseSync for the MacTCP driver (.IPP)
    if (gMacTCPRefNum != 0) {
         LogToDialog("MacTCP driver (RefNum: %d) remains open by design.", gMacTCPRefNum);
        gMacTCPRefNum = 0; // Still reset our global reference number variable
    } else {
        LogToDialog("MacTCP driver was not open.");
    }

    LogToDialog("Networking cleanup complete.");
}


// --- UDP Discovery Functions ---
// ... (InitUDPDiscovery remains the same) ...
OSErr InitUDPDiscovery(void) {
    OSErr err;
    UDPiopb udpPB; // Use UDPiopb for udpCreate

    LogToDialog("Initializing UDP Discovery Endpoint (Create/Read)...");

    if (gMacTCPRefNum == 0) {
        LogToDialog("Error: MacTCP driver not open.");
        return notOpenErr;
    }

    // --- Allocate Receive Buffer Dynamically --- ADDED
    LogToDialog("Allocating UDP receive buffer (size: %d)...", kUDPReceiveBufferSize);
    gUDPReceiveAreaPtr = NewPtr(kUDPReceiveBufferSize);
    if (gUDPReceiveAreaPtr == NULL) {
        err = MemError();
        LogToDialog("Error: NewPtr failed for UDP receive buffer. Error: %d", err);
        return err; // Return memory error
    }
    LogToDialog("UDP receive buffer allocated at 0x%lX.", (unsigned long)gUDPReceiveAreaPtr);

    // --- Create UDP Endpoint ---
    memset(&udpPB, 0, sizeof(UDPiopb)); // Use UDPiopb
    udpPB.ioCRefNum = gMacTCPRefNum;
    udpPB.csCode = udpCreate;

    // Use the named fields within the 'create' union member
    udpPB.csParam.create.rcvBuff = gUDPReceiveAreaPtr;
    udpPB.csParam.create.rcvBuffLen = kUDPReceiveBufferSize;
    udpPB.csParam.create.notifyProc = NULL;
    udpPB.csParam.create.localPort = PORT_UDP; // Use specific port
    udpPB.csParam.create.userDataPtr = NULL;

    LogToDialog("Calling PBControlSync (udpCreate) for port %u with rcvBufLen=%d...", PORT_UDP, kUDPReceiveBufferSize);
    err = PBControlSync((ParmBlkPtr)&udpPB);

    if (err != noErr) {
        LogToDialog("Error: PBControlSync(udpCreate) failed. Error: %d", err);
        sUDPEndpoint = NULL;
        if (gUDPReceiveAreaPtr != NULL) {
            DisposePtr(gUDPReceiveAreaPtr);
            gUDPReceiveAreaPtr = NULL;
        }
        return err;
    }

    // Retrieve the output StreamPtr
    sUDPEndpoint = udpPB.udpStream;
    unsigned short assignedPort = udpPB.csParam.create.localPort;
    LogToDialog("UDP Endpoint created successfully (StreamPtr: 0x%lX) on port %u.", (unsigned long)sUDPEndpoint, assignedPort);
    gLastBroadcastTimeTicks = 0; // Reset broadcast timer

    // --- Create Completion Routine UPP --- ADDED
    LogToDialog("Creating UDP Read Completion UPP...");
    gUDPReadCompletionUPP = NewUDPIOCompletionProc(UDPReadCompletion);
    if (gUDPReadCompletionUPP == NULL) {
        LogToDialog("Error: NewUDPIOCompletionProc failed!");
        CleanupNetworking(); // Attempt full cleanup
        return memFullErr; // Or another appropriate error
    }
    LogToDialog("UDP Read Completion UPP created.");

    // Issue the first asynchronous read
    err = IssueUDPRead();
    if (err != noErr) {
        LogToDialog("Error: Failed to issue initial UDP read. Error: %d", err);
        CleanupNetworking(); // Attempt full cleanup
        return err;
    }

    return noErr;
}

// ... (SendDiscoveryBroadcast remains the same) ...
OSErr SendDiscoveryBroadcast(void) {
    OSErr err;
    char buffer[BUFFER_SIZE];
    struct wdsEntry wds[2];
    UDPiopb udpPB; // Use UDPiopb for udpSend

    if (sUDPEndpoint == NULL) {
        return invalidStreamPtr;
    }

    err = format_message(buffer, BUFFER_SIZE, MSG_DISCOVERY, gMyUsername, gMyLocalIPStr, "");
    if (err != 0) {
        LogToDialog("Error: Failed to format discovery broadcast message.");
        return paramErr;
    }

    wds[0].length = strlen(buffer);
    wds[0].ptr = buffer;
    wds[1].length = 0;
    wds[1].ptr = NULL;

    memset(&udpPB, 0, sizeof(UDPiopb)); // Use UDPiopb
    udpPB.ioCRefNum = gMacTCPRefNum;
    udpPB.csCode = udpSend;
    udpPB.udpStream = sUDPEndpoint; // Set stream pointer directly

    // Use the named fields within the 'send' union member
    udpPB.csParam.send.remoteHost = BROADCAST_IP;
    udpPB.csParam.send.remotePort = PORT_UDP;
    udpPB.csParam.send.wdsPtr = (Ptr)wds;
    udpPB.csParam.send.checkSum = true; // Boolean field
    udpPB.csParam.send.userDataPtr = NULL;
    // udpPB.csParam.send.sendLength = 0; // This is an output field

    err = PBControlSync((ParmBlkPtr)&udpPB);

    if (err != noErr) {
        LogToDialog("Error: PBControlSync(udpSend) broadcast failed. Error: %d", err);
        return err;
    }

    gLastBroadcastTimeTicks = TickCount();
    return noErr;
}

// ... (CheckSendBroadcast remains the same) ...
void CheckSendBroadcast(void) {
    unsigned long currentTimeTicks = TickCount();
    const unsigned long intervalTicks = (unsigned long)DISCOVERY_INTERVAL * 60;

    if (sUDPEndpoint == NULL) return;

    if (gLastBroadcastTimeTicks == 0 || (currentTimeTicks - gLastBroadcastTimeTicks) >= intervalTicks) {
        SendDiscoveryBroadcast();
    }
}

// ... (IssueUDPRead remains the same) ...
OSErr IssueUDPRead(void) {
    OSErr err;

    if (sUDPEndpoint == NULL) {
        return invalidStreamPtr;
    }
    if (gUDPReadPending) {
        return noErr;
    }
    if (gUDPReceiveAreaPtr == NULL) {
        LogToDialog("Error: Cannot issue UDPRead, receive buffer not allocated.");
        return memFullErr;
    }
    // --- Use global UPP --- MODIFIED
    if (gUDPReadCompletionUPP == NULL) {
        LogToDialog("Error: Cannot issue UDPRead, completion UPP is NULL.");
        return paramErr; // Indicate a setup problem
    }

    memset(&gUDPReadPB, 0, sizeof(UDPiopb));
    gUDPReadPB.ioCompletion = gUDPReadCompletionUPP; // Use the global UPP
    gUDPReadPB.ioCRefNum = gMacTCPRefNum;
    gUDPReadPB.csCode = udpRead;
    gUDPReadPB.udpStream = sUDPEndpoint;

    gUDPReadPB.csParam.receive.timeOut = 0;
    gUDPReadPB.csParam.receive.rcvBuff = gUDPReceiveAreaPtr;
    gUDPReadPB.csParam.receive.rcvBuffLen = kUDPReceiveBufferSize;
    gUDPReadPB.csParam.receive.userDataPtr = NULL;

    err = PBControlAsync((ParmBlkPtr)&gUDPReadPB);

    if (err == noErr) {
        gUDPReadPending = true;
    } else {
        LogToDialog("Error: PBControlAsync(udpRead) failed to issue. Error: %d", err);
        gUDPReadPending = false;
        // Don't dispose the global UPP here on failure to issue
    }
    return err;
}

// ... (UDPReadCompletion remains the same) ...
pascal void UDPReadCompletion(UDPiopb *pb) {
    #pragma unused(pb)
    gUDPReadPending = false;
}


/**
 * @brief Processes the result of a completed UDPRead operation.
 * @details Checks the result, parses valid packets, updates peer list,
 *          sets flags for deferred operations (AddrToStr, SendResponse),
 *          returns the buffer, and re-issues the read.
 */
void ProcessUDPReceive(void) {
    OSErr result;
    ip_addr remoteIP;
    udp_port remotePort;
    Ptr dataPtr;
    unsigned short dataLen;
    char sender_ip_str[INET_ADDRSTRLEN]; // Buffer for AddrToStr result
    char parsed_sender_ip[INET_ADDRSTRLEN]; // Buffer for IP parsed from message
    char sender_username[32];
    char msg_type[32];
    char content[BUFFER_SIZE];
    OSErr err;
    UDPiopb bfrPB;

    if (gUDPReadPending) {
        return; // Read hasn't completed yet
    }

    result = gUDPReadPB.ioResult;

    if (result == noErr) {
        // Extract data from the completed PB
        remoteIP = gUDPReadPB.csParam.receive.remoteHost;
        remotePort = gUDPReadPB.csParam.receive.remotePort;
        dataPtr = gUDPReadPB.csParam.receive.rcvBuff;
        dataLen = gUDPReadPB.csParam.receive.rcvBuffLen;

        if (dataPtr != gUDPReceiveAreaPtr) {
             LogToDialog("Warning: Received data pointer (0x%lX) doesn't match allocated buffer (0x%lX)!", (unsigned long)dataPtr, (unsigned long)gUDPReceiveAreaPtr);
             goto return_buffer_and_reissue; // Still need to return buffer
        }

        // Null-terminate the received data
        if (dataLen < kUDPReceiveBufferSize) {
            gUDPReceiveAreaPtr[dataLen] = '\0';
        } else {
            gUDPReceiveAreaPtr[kUDPReceiveBufferSize - 1] = '\0';
        }

        // --- Defer AddrToStr ---
        // We need the IP string for logging and peer management.
        // For now, we'll risk calling AddrToStr here, but ideally, this
        // should also be deferred if it proves unstable.
        AddrToStr(remoteIP, sender_ip_str);
        if (sender_ip_str[0] == '\0') { // Basic check if AddrToStr failed
            strcpy(sender_ip_str, "?.?.?.?");
        }

        // Ignore messages from self
        if (strcmp(sender_ip_str, gMyLocalIPStr) == 0) {
             goto return_buffer_and_reissue; // Skip processing, just return buffer and reissue read
        }

        LogToDialog("UDP Data Received (%d bytes) from %s:%d", dataLen, sender_ip_str, remotePort);

        // Parse the message
        // Pass sender_ip_str as output buffer for parsed IP, though we primarily use the AddrToStr result
        if (parse_message(gUDPReceiveAreaPtr, parsed_sender_ip, sender_username, msg_type, content) == 0) {
            // Use the IP from AddrToStr (sender_ip_str) for logging and peer management
            LogToDialog("Parsed UDP Msg: Type='%s', Sender='%s', IP='%s', Content='%s'",
                        msg_type, sender_username, sender_ip_str, content);

            if (strcmp(msg_type, MSG_DISCOVERY) == 0) {
                LogToDialog("Received DISCOVERY from %s@%s", sender_username, sender_ip_str);
                // --- Defer SendUDPResponse ---
                gNeedToSendResponse = true;
                gResponseDestIP = remoteIP;
                gResponseDestPort = remotePort;
                // Add/Update peer immediately (assumed safe)
                AddOrUpdatePeer(sender_ip_str, sender_username);
            }
            else if (strcmp(msg_type, MSG_DISCOVERY_RESPONSE) == 0) {
                LogToDialog("Received DISCOVERY_RESPONSE from %s@%s", sender_username, sender_ip_str);
                // Add/Update peer immediately (assumed safe)
                AddOrUpdatePeer(sender_ip_str, sender_username);
            }
            else {
                 LogToDialog("Received unknown UDP message type: %s", msg_type);
            }
        } else {
            LogToDialog("Failed to parse UDP message from %s: %s", sender_ip_str, gUDPReceiveAreaPtr);
        }

return_buffer_and_reissue:
        // Return the buffer to MacTCP (Synchronous call - potential risk)
        memset(&bfrPB, 0, sizeof(UDPiopb));
        bfrPB.ioCRefNum = gMacTCPRefNum;
        bfrPB.csCode = udpBfrReturn;
        bfrPB.udpStream = sUDPEndpoint;
        bfrPB.csParam.receive.rcvBuff = dataPtr; // Pointer to the buffer we received
        err = PBControlSync((ParmBlkPtr)&bfrPB);
        if (err != noErr) {
             LogToDialog("Error: PBControlSync(udpBfrReturn) failed. Error: %d", err);
             // If buffer return fails, MacTCP might run out of buffers.
             // Maybe we should stop trying to read? For now, just log.
        }

    } else if (result != 1) { // Ignore inProgress (1)
        LogToDialog("UDP Read completed with error: %d. Re-issuing.", result);
    }

    // Re-issue the asynchronous read regardless of previous result (unless fatal error)
    if (result != connectionTerminated && result != invalidStreamPtr) {
       IssueUDPRead();
    } else {
       LogToDialog("UDP Stream seems terminated (Error: %d). Not re-issuing read.", result);
    }
}

/**
 * @brief Sends a UDP discovery response back to a specific sender. (Synchronous)
 */
OSErr SendUDPResponse(ip_addr destIP, udp_port destPort) {
    // ... (This function remains the same as before, using PBControlSync) ...
    OSErr err;
    char buffer[BUFFER_SIZE];
    struct wdsEntry wds[2];
    UDPiopb udpPB; // Use UDPiopb for udpSend

    if (sUDPEndpoint == NULL) {
        LogToDialog("Error: Cannot send response, UDP endpoint not initialized.");
        return invalidStreamPtr;
    }

    err = format_message(buffer, BUFFER_SIZE, MSG_DISCOVERY_RESPONSE, gMyUsername, gMyLocalIPStr, "");
    if (err != 0) {
        LogToDialog("Error: Failed to format discovery response message.");
        return paramErr;
    }

    wds[0].length = strlen(buffer);
    wds[0].ptr = buffer;
    wds[1].length = 0;
    wds[1].ptr = NULL;

    memset(&udpPB, 0, sizeof(UDPiopb)); // Use UDPiopb
    udpPB.ioCRefNum = gMacTCPRefNum;
    udpPB.csCode = udpSend;
    udpPB.udpStream = sUDPEndpoint;

    udpPB.csParam.send.remoteHost = destIP;
    udpPB.csParam.send.remotePort = destPort;
    udpPB.csParam.send.wdsPtr = (Ptr)wds;
    udpPB.csParam.send.checkSum = true;
    udpPB.csParam.send.userDataPtr = NULL;

    err = PBControlSync((ParmBlkPtr)&udpPB);

    if (err != noErr) {
        LogToDialog("Error: PBControlSync(udpSend) response failed. Error: %d", err);
        return err;
    }

    LogToDialog("Discovery response sent to %s:%d.", gMyLocalIPStr, destPort); // Log using local IP for now
    return noErr;
}

/**
 * @brief Checks if a deferred UDP response needs to be sent and sends it.
 * @details Called from the main event loop during a safe time.
 */
void CheckAndSendDeferredResponse(void) {
    if (gNeedToSendResponse) {
        LogToDialog("Processing deferred UDP response to %lu:%u", gResponseDestIP, gResponseDestPort);
        SendUDPResponse(gResponseDestIP, gResponseDestPort);
        gNeedToSendResponse = false; // Clear the flag
    }
}


// --- Peer Management Functions ---
// ... (InitPeerList, AddOrUpdatePeer, FindPeerByIP, PruneInactivePeers remain the same) ...
/**
 * @brief Initializes the global peer list.
 */
void InitPeerList(void) {
    LogToDialog("Initializing peer list (Max: %d)", MAX_PEERS);
    memset(gPeerList, 0, sizeof(gPeerList)); // Zero out the entire list
    gPeerCount = 0;
}

/**
 * @brief Adds a new peer or updates an existing one.
 */
int AddOrUpdatePeer(const char *ip, const char *username) {
    short i;
    unsigned long nowTicks = TickCount();

    // Prune inactive peers first to make space if needed
    PruneInactivePeers();

    // Check if peer already exists
    short existingIndex = FindPeerByIP(ip);

    if (existingIndex != -1) {
        // Peer found, update last_seen and potentially username
        gPeerList[existingIndex].last_seen_ticks = nowTicks;
        if (username && username[0] != '\0' && strcmp(gPeerList[existingIndex].username, username) != 0) {
             LogToDialog("Updating username for peer %s to %s", ip, username);
             strncpy(gPeerList[existingIndex].username, username, 31);
             gPeerList[existingIndex].username[31] = '\0';
             // TODO: Update UI list if implemented
        }
        // LogToDialog("Updated peer %s@%s", gPeerList[existingIndex].username, ip);
        return 0; // Indicate update
    }

    // Peer not found, try to add to an inactive slot
    for (i = 0; i < MAX_PEERS; i++) {
        if (!gPeerList[i].active) {
            strncpy(gPeerList[i].ip, ip, INET_ADDRSTRLEN - 1);
            gPeerList[i].ip[INET_ADDRSTRLEN - 1] = '\0';
            strncpy(gPeerList[i].username, username ? username : "???", 31);
            gPeerList[i].username[31] = '\0';
            gPeerList[i].last_seen_ticks = nowTicks;
            gPeerList[i].active = 1;
            gPeerCount++; // Increment active count
            LogToDialog("Added new peer %d: %s@%s", gPeerCount, gPeerList[i].username, ip);
            // TODO: Update UI list if implemented
            return 1; // Indicate new peer added
        }
    }

    // If loop finishes, list is full
    LogToDialog("Peer list full. Cannot add peer %s@%s.", username ? username : "???", ip);
    return -1; // Indicate list full
}

/**
 * @brief Finds a peer by IP address.
 * @return Index of the peer if found and active, otherwise -1.
 */
short FindPeerByIP(const char *ip) {
    short i;
    for (i = 0; i < MAX_PEERS; i++) {
        if (gPeerList[i].active && strcmp(gPeerList[i].ip, ip) == 0) {
            return i;
        }
    }
    return -1; // Not found
}

/**
 * @brief Marks peers as inactive if they haven't been seen recently.
 */
void PruneInactivePeers(void) {
    unsigned long nowTicks = TickCount();
    const unsigned long timeoutTicks = (unsigned long)PEER_TIMEOUT * 60;
    short i;
    Boolean changed = false;

    for (i = 0; i < MAX_PEERS; i++) {
        if (gPeerList[i].active) {
            if ((nowTicks - gPeerList[i].last_seen_ticks) >= timeoutTicks) {
                LogToDialog("Peer %s@%s timed out.", gPeerList[i].username, gPeerList[i].ip);
                gPeerList[i].active = 0; // Mark as inactive
                gPeerCount--; // Decrement active count
                changed = true;
            }
        }
    }
    if (changed) {
        // TODO: Update UI list if implemented
    }
}