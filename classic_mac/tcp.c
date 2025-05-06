//====================================
// FILE: ./classic_mac/tcp.c
//====================================

#include "tcp.h"
#include "logging.h"
#include "protocol.h"
#include "peer_mac.h"
#include "dialog.h" // For updating UI if needed
#include "dialog_peerlist.h"
#include "network.h" // For gMacTCPRefNum, gMyUsername, gMyLocalIPStr, ParseIPv4, YieldTimeToSystem, AddrToStr
#include "../shared/messaging_logic.h"
#include <Devices.h>
#include <Errors.h>
#include <MacTypes.h> // Ensure Byte is available
#include <Memory.h> // For NewPtrClear, DisposePtr, BlockMoveData
#include <string.h> // For memset, strcmp, strlen
#include <stdio.h>  // For sprintf
#include <stdlib.h> // For size_t if needed
#include <Events.h> // For TickCount, Delay
#include <OSUtils.h> // For Delay

// --- Constants ---
#define kTCPRecvBufferSize 8192
#define kTCPInternalBufferSize 8192
#define kConnectTimeoutTicks 300
#define kSendTimeoutTicks 180
#define kCloseTimeoutTicks 120
#define kAbortTimeoutTicks 60
#define kKillWaitTimeoutTicks 180
#define kQuitLoopDelayTicks 120
#define kErrorRetryDelayTicks 120

#define kMacTCPTimeoutErr (-23016)
#define AbortTrue 1

// --- Globals ---
static StreamPtr gTCPStream = NULL;
static Ptr gTCPInternalBuffer = NULL;
static Ptr gTCPRecvBuffer = NULL;
static TCPState gTCPState = TCP_STATE_UNINITIALIZED;
static TCPiopb gListenPB;
static TCPiopb gRecvPB;
static TCPiopb gClosePB;
static TCPiopb *gCurrentPendingPB = NULL;
static Boolean gIsSending = false;
volatile static Boolean gNeedToStartListen = false;
volatile static Boolean gNeedToStartRecv = false;
volatile static Boolean gNeedToProcessData = false;
volatile static Boolean gNeedToCloseIncoming = false;
volatile static unsigned short gRcvDataLength = 0;
static ip_addr gPeerIP = 0;
static tcp_port gPeerPort = 0;

// --- Forward Declarations ---
static void ProcessTCPReceive(unsigned short dataLength);
static OSErr StartAsyncListen(void);
static OSErr StartAsyncRecv(void);
static OSErr StartAsyncCloseIncoming(void);
static OSErr KillAsyncOperation(GiveTimePtr giveTime);
static OSErr LowLevelSyncPoll(TCPiopb *pBlock, GiveTimePtr giveTime, SInt16 csCode);
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtr, Ptr connectionBuffer, unsigned long connBufferLen);
static OSErr LowTCPActiveOpenSyncPoll(SInt8 ulpTimeoutTicks, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime);
static OSErr LowTCPSendSyncPoll(SInt8 ulpTimeoutTicks, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime);
static OSErr LowTCPAbortSyncPoll(GiveTimePtr giveTime);
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamPtr);

// --- Shared Logic Callbacks (implementation unchanged) ---
static int mac_tcp_add_or_update_peer(const char* ip, const char* username, void* platform_context) { (void)platform_context; int addResult = AddOrUpdatePeer(ip, username); if (addResult > 0) { log_message("Peer connected/updated via TCP: %s@%s", username, ip); if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true); } else if (addResult < 0) { log_message("Peer list full, could not add/update %s@%s from TCP connection", username, ip); } return addResult; }
static void mac_tcp_display_text_message(const char* username, const char* ip, const char* message_content, void* platform_context) { (void)platform_context; (void)ip; char displayMsg[BUFFER_SIZE + 100]; if (gMainWindow != NULL && gMessagesTE != NULL && gDialogTEInitialized) { sprintf(displayMsg, "%s: %s", username ? username : "???", message_content ? message_content : ""); AppendToMessagesTE(displayMsg); AppendToMessagesTE("\r"); log_message("Message from %s@%s: %s", username, ip, message_content); } else { log_message("Error (mac_tcp_display_text_message): Cannot display message, dialog not ready."); } }
static void mac_tcp_mark_peer_inactive(const char* ip, void* platform_context) { (void)platform_context; if (!ip) return; log_message("Peer %s has sent QUIT notification via TCP.", ip); if (MarkPeerInactive(ip)) { if (gMainWindow != NULL && gPeerListHandle != NULL) UpdatePeerDisplayList(true); } }

// --- Completion Routines (Pascal Convention) ---

// ListenCompleteProc remains unchanged
pascal void ListenCompleteProc(struct TCPiopb *iopb) {
    OSErr ioResult = iopb->ioResult; gCurrentPendingPB = NULL;
    if (ioResult == noErr) { gPeerIP = iopb->csParam.open.remoteHost; gPeerPort = iopb->csParam.open.remotePort; gTCPState = TCP_STATE_CONNECTED_IN; gNeedToStartRecv = true; }
    else { const char *errMsg = "Unknown Error"; Boolean isReqAborted = (ioResult == reqAborted); if (isReqAborted) errMsg = "reqAborted (-23015)"; else if (ioResult == connectionExists) errMsg = "connectionExists (-23007)"; else if (ioResult == invalidStreamPtr) errMsg = "invalidStreamPtr (-23010)"; else if (ioResult == commandTimeout) errMsg = "commandTimeout (-23016)";
        if (!isReqAborted) { log_message("ListenCompleteProc: Error: %d (%s)", ioResult, errMsg); } else { log_to_file_only("ListenCompleteProc: Request aborted (%d).", ioResult); }
        gTCPState = TCP_STATE_IDLE; if (ioResult != invalidStreamPtr && !isReqAborted) { gNeedToStartListen = true; } else if (ioResult == invalidStreamPtr) { gTCPState = TCP_STATE_ERROR; log_message("ListenCompleteProc: CRITICAL - Stream invalid."); } }
}

// RecvCompleteProc remains unchanged
pascal void RecvCompleteProc(struct TCPiopb *iopb) {
    OSErr ioResult = iopb->ioResult; gCurrentPendingPB = NULL;
    if (ioResult == noErr) { gRcvDataLength = iopb->csParam.receive.rcvBuffLen; gNeedToProcessData = true; }
    else if (ioResult == connectionClosing) { gRcvDataLength = iopb->csParam.receive.rcvBuffLen; gNeedToProcessData = true; gNeedToCloseIncoming = false; }
    else { gRcvDataLength = 0; const char *errMsg = "Unknown Error"; Boolean isReqAborted = (ioResult == reqAborted); if (isReqAborted) errMsg = "reqAborted (-23015)"; else if (ioResult == connectionTerminated) errMsg = "connectionTerminated (-23012)"; else if (ioResult == invalidStreamPtr) errMsg = "invalidStreamPtr (-23010)";
        if (!isReqAborted) { log_message("RecvCompleteProc: Error: %d (%s)", ioResult, errMsg); } else { log_to_file_only("RecvCompleteProc: Request aborted (%d).", ioResult); }
        if (ioResult == invalidStreamPtr) { gTCPState = TCP_STATE_ERROR; log_message("RecvCompleteProc: CRITICAL - Stream invalid."); } else if (!isReqAborted) { gNeedToCloseIncoming = true; } }
}

// CloseCompleteProc remains unchanged
pascal void CloseCompleteProc(struct TCPiopb *iopb) {
    OSErr ioResult = iopb->ioResult; gCurrentPendingPB = NULL;
    if (ioResult != noErr) { log_message("CloseCompleteProc: Warning - Async close failed: %d", ioResult); } else { log_to_file_only("CloseCompleteProc: Async close completed."); }
    gTCPState = TCP_STATE_IDLE; gNeedToStartListen = true;
}


// --- Public Functions ---

// InitTCP remains unchanged
OSErr InitTCP(short macTCPRefNum) { OSErr err; log_message("Initializing Single TCP Stream (Async Strategy)..."); if (macTCPRefNum == 0) return paramErr; if (gTCPStream != NULL || gTCPState != TCP_STATE_UNINITIALIZED) { log_message("Error (InitTCP): Already initialized or in unexpected state (%d)?", gTCPState); return streamAlreadyOpen; }
    gTCPInternalBuffer = NewPtrClear(kTCPInternalBufferSize); gTCPRecvBuffer = NewPtrClear(kTCPRecvBufferSize); if (gTCPInternalBuffer == NULL || gTCPRecvBuffer == NULL) { log_message("Fatal Error: Could not allocate TCP buffers."); if (gTCPInternalBuffer) DisposePtr(gTCPInternalBuffer); if (gTCPRecvBuffer) DisposePtr(gTCPRecvBuffer); gTCPInternalBuffer = gTCPRecvBuffer = NULL; return memFullErr; } log_message("Allocated TCP buffers (Internal: %ld, Recv: %ld).", (long)kTCPInternalBufferSize, (long)kTCPRecvBufferSize);
    log_message("Creating Single Stream..."); err = LowTCPCreateSync(macTCPRefNum, &gTCPStream, gTCPInternalBuffer, kTCPInternalBufferSize); if (err != noErr || gTCPStream == NULL) { log_message("Error: Failed to create TCP Stream: %d", err); CleanupTCP(macTCPRefNum); return err; } log_message("Single TCP Stream created (0x%lX).", (unsigned long)gTCPStream);
    gTCPState = TCP_STATE_IDLE; gCurrentPendingPB = NULL; gIsSending = false; gNeedToStartListen = true; gNeedToStartRecv = false; gNeedToProcessData = false; gNeedToCloseIncoming = false; gPeerIP = 0; gPeerPort = 0; log_message("TCP initialization complete. State: IDLE. Will start listening on next poll."); return noErr; }

// CleanupTCP remains unchanged
void CleanupTCP(short macTCPRefNum) { log_message("Cleaning up Single TCP Stream (Async Strategy)..."); StreamPtr streamToRelease = gTCPStream; TCPState stateBeforeCleanup = gTCPState; gTCPStream = NULL; gTCPState = TCP_STATE_RELEASING;
    if (gCurrentPendingPB != NULL && stateBeforeCleanup != TCP_STATE_IDLE) { log_message("Cleanup: Attempting async kill of pending operation (State: %d)...", stateBeforeCleanup); OSErr killErr = PBKillIO((ParmBlkPtr)gCurrentPendingPB, false); if (killErr != noErr) { log_message("Warning: PBKillIO failed immediately during cleanup: %d", killErr); } gCurrentPendingPB = NULL; }
    if (stateBeforeCleanup == TCP_STATE_CONNECTED_IN || stateBeforeCleanup == TCP_STATE_RECEIVING) { log_message("Cleanup: Attempting synchronous abort (best effort)..."); LowTCPAbortSyncPoll(YieldTimeToSystem); }
    if (streamToRelease != NULL && macTCPRefNum != 0) { log_message("Attempting sync release of stream 0x%lX...", (unsigned long)streamToRelease); OSErr relErr = LowTCPReleaseSync(macTCPRefNum, streamToRelease); if (relErr != noErr) log_message("Warning: Sync release failed: %d", relErr); else log_to_file_only("Sync release successful."); } else if (streamToRelease != NULL) { log_message("Warning: Cannot release stream, MacTCP refnum is 0."); }
    gTCPState = TCP_STATE_UNINITIALIZED; gCurrentPendingPB = NULL; gIsSending = false; gNeedToStartListen = false; gNeedToStartRecv = false; gNeedToProcessData = false; gNeedToCloseIncoming = false; gPeerIP = 0; gPeerPort = 0;
    if (gTCPRecvBuffer != NULL) { DisposePtr(gTCPRecvBuffer); gTCPRecvBuffer = NULL; } if (gTCPInternalBuffer != NULL) { DisposePtr(gTCPInternalBuffer); gTCPInternalBuffer = NULL; } log_message("TCP cleanup finished."); }

// PollTCP remains unchanged
void PollTCP(void) { OSErr err; unsigned long dummyTimer; if (gTCPStream == NULL || gTCPState == TCP_STATE_UNINITIALIZED || gTCPState == TCP_STATE_ERROR || gTCPState == TCP_STATE_RELEASING) return; if (gIsSending) return; if (gCurrentPendingPB != NULL) return;
    if (gNeedToStartListen && gTCPState == TCP_STATE_IDLE) { gNeedToStartListen = false; log_to_file_only("PollTCP: Starting async listen..."); err = StartAsyncListen(); if (err != noErr && err != 1) { log_message("PollTCP: Failed to start async listen: %d. Retrying later.", err); gTCPState = TCP_STATE_IDLE; gNeedToStartListen = true; Delay(kErrorRetryDelayTicks, &dummyTimer); } return; }
    if (gNeedToStartRecv && gTCPState == TCP_STATE_CONNECTED_IN) { gNeedToStartRecv = false; log_to_file_only("PollTCP: Starting async receive..."); err = StartAsyncRecv(); if (err != noErr && err != 1) { log_message("PollTCP: Failed to start async receive: %d. Closing connection.", err); gTCPState = TCP_STATE_CONNECTED_IN; gNeedToCloseIncoming = true; } return; }
    if (gNeedToProcessData) { gNeedToProcessData = false; log_to_file_only("PollTCP: Processing received data (%u bytes)...", gRcvDataLength); ProcessTCPReceive(gRcvDataLength); gRcvDataLength = 0;
         if (gTCPState == TCP_STATE_RECEIVING) { log_to_file_only("PollTCP: Starting next async receive..."); err = StartAsyncRecv(); if (err != noErr && err != 1) { log_message("PollTCP: Failed to start next async receive: %d. Closing connection.", err); gTCPState = TCP_STATE_RECEIVING; gNeedToCloseIncoming = true; } }
         else if (gTCPState == TCP_STATE_CONNECTED_IN) { char peerIPStr[INET_ADDRSTRLEN]; AddrToStr(gPeerIP, peerIPStr); log_message("PollTCP: Peer %s closed connection gracefully. Returning to IDLE.", peerIPStr); gTCPState = TCP_STATE_IDLE; gNeedToStartListen = true; }
         else { log_message("PollTCP: WARNING - Processed data but state is unexpected: %d", gTCPState); gTCPState = TCP_STATE_IDLE; gNeedToStartListen = true; } return; }
    if (gNeedToCloseIncoming) { gNeedToCloseIncoming = false; if (gTCPState == TCP_STATE_RECEIVING || gTCPState == TCP_STATE_CONNECTED_IN) { log_message("PollTCP: Closing incoming connection gracefully..."); err = StartAsyncCloseIncoming(); if (err != noErr && err != 1) { log_message("PollTCP: Failed to start async close: %d. Assuming closed, returning to IDLE.", err); gTCPState = TCP_STATE_IDLE; gNeedToStartListen = true; } }
         else { log_message("PollTCP: WARNING - NeedToCloseIncoming flag set, but state is unexpected: %d. Returning to IDLE.", gTCPState); gTCPState = TCP_STATE_IDLE; gNeedToStartListen = true; } return; }
}

// GetTCPState remains unchanged
TCPState GetTCPState(void) { return gTCPState; }

// WaitForKillCompletion remains unchanged
static OSErr WaitForKillCompletion(GiveTimePtr giveTime, const char* context) { unsigned long startTime = TickCount(); log_to_file_only("WaitForKillCompletion (%s): Waiting for state to return to IDLE...", context); while ((TickCount() - startTime) < kKillWaitTimeoutTicks) { if (gTCPState == TCP_STATE_IDLE && gCurrentPendingPB == NULL) { log_to_file_only("WaitForKillCompletion (%s): Kill completed (State IDLE).", context); return noErr; } giveTime(); } log_message("Error (%s): Timeout waiting for PBKillIO to complete. State: %d", context, gTCPState); return reqAborted; }

// TCP_SendTextMessageSync remains unchanged
OSErr TCP_SendTextMessageSync(const char *peerIP, const char *message, GiveTimePtr giveTime) { OSErr err = noErr, finalErr = noErr; ip_addr targetIP = 0; char messageBuffer[BUFFER_SIZE]; int formattedLen; struct wdsEntry sendWDS[2];
    log_to_file_only("TCP_SendTextMessageSync: Request to send TEXT to %s", peerIP); if (gMacTCPRefNum == 0) return notOpenErr; if (gTCPStream == NULL) return invalidStreamPtr; if (peerIP == NULL || message == NULL || giveTime == NULL) return paramErr;
    if (gIsSending) { log_message("Warning (SendText): Send already in progress."); return streamBusyErr; } gIsSending = true; log_to_file_only("TCP_SendTextMessageSync: Acquired send lock.");
    err = KillAsyncOperation(giveTime); if (err != noErr) { log_message("Error (SendText): Failed to kill pending async operation (%d). Cannot send.", err); finalErr = streamBusyErr; goto SendTextCleanup; } if (gTCPState != TCP_STATE_IDLE) { log_message("Error (SendText): State not IDLE (%d) after kill attempt. Cannot send.", gTCPState); finalErr = streamBusyErr; goto SendTextCleanup; }
    err = ParseIPv4(peerIP, &targetIP); if (err != noErr || targetIP == 0) { log_message("Error (SendText): Could not parse peer IP '%s'.", peerIP); finalErr = paramErr; goto SendTextCleanup; }
    formattedLen = format_message(messageBuffer, BUFFER_SIZE, MSG_TEXT, gMyUsername, gMyLocalIPStr, message); if (formattedLen <= 0) { log_message("Error (SendText): Failed to format TEXT message for %s.", peerIP); finalErr = paramErr; goto SendTextCleanup; }
    log_to_file_only("SendText: Connecting..."); err = LowTCPActiveOpenSyncPoll(kConnectTimeoutTicks, targetIP, PORT_TCP, giveTime);
    if (err == noErr) { log_to_file_only("SendText: Connected successfully."); sendWDS[0].length = formattedLen; sendWDS[0].ptr = (Ptr)messageBuffer; sendWDS[1].length = 0; sendWDS[1].ptr = NULL; log_to_file_only("SendText: Sending data..."); err = LowTCPSendSyncPoll(kSendTimeoutTicks, true, (Ptr)sendWDS, giveTime); if (err == noErr) { log_to_file_only("SendText: Send successful."); } else { log_message("Error (SendText): Send failed: %d", err); finalErr = err; } log_to_file_only("SendText: Aborting connection..."); OSErr abortErr = LowTCPAbortSyncPoll(giveTime); if (abortErr != noErr) { log_message("Warning (SendText): Abort failed: %d", abortErr); if (finalErr == noErr) finalErr = abortErr; } }
    else { log_message("Error (SendText): Connect failed: %d", err); finalErr = err; }
SendTextCleanup: gNeedToStartListen = true; gIsSending = false; log_to_file_only("TCP_SendTextMessageSync: Released send lock. Final Status: %d.", finalErr); return finalErr; }

// TCP_SendQuitMessagesSync remains unchanged
OSErr TCP_SendQuitMessagesSync(GiveTimePtr giveTime) { int i; OSErr lastErr = noErr, currentErr = noErr; char quitMessageBuffer[BUFFER_SIZE]; int formattedLen, activePeerCount = 0, sentCount = 0; unsigned long dummyTimer; struct wdsEntry sendWDS[2];
    log_message("TCP_SendQuitMessagesSync: Starting..."); if (gMacTCPRefNum == 0) return notOpenErr; if (gTCPStream == NULL) return invalidStreamPtr; if (giveTime == NULL) return paramErr;
    if (gIsSending) { log_message("Warning (SendQuit): Send already in progress."); return streamBusyErr; } gIsSending = true; log_to_file_only("TCP_SendQuitMessagesSync: Acquired send lock.");
    currentErr = KillAsyncOperation(giveTime); if (currentErr != noErr) { log_message("Error (SendQuit): Failed to kill pending async operation (%d). Cannot send.", currentErr); lastErr = streamBusyErr; goto SendQuitCleanup; } if (gTCPState != TCP_STATE_IDLE) { log_message("Error (SendQuit): State not IDLE (%d) after kill attempt. Cannot send.", gTCPState); lastErr = streamBusyErr; goto SendQuitCleanup; }
    formattedLen = format_message(quitMessageBuffer, BUFFER_SIZE, MSG_QUIT, gMyUsername, gMyLocalIPStr, ""); if (formattedLen <= 0) { log_message("Error (SendQuit): Failed to format QUIT message."); lastErr = paramErr; goto SendQuitCleanup; }
    for (i = 0; i < MAX_PEERS; ++i) if (gPeerManager.peers[i].active) activePeerCount++; log_message("TCP_SendQuitMessagesSync: Found %d active peers to notify.", activePeerCount); if (activePeerCount == 0) { lastErr = noErr; goto SendQuitCleanup; }
    for (i = 0; i < MAX_PEERS; ++i) { if (gPeerManager.peers[i].active) { ip_addr currentTargetIP = 0; currentErr = noErr; if (gTCPState != TCP_STATE_IDLE) { log_message("CRITICAL (SendQuit): State not IDLE (%d) before processing peer %s. Aborting loop.", gTCPState, gPeerManager.peers[i].ip); lastErr = ioErr; break; } log_message("TCP_SendQuitMessagesSync: Attempting QUIT to %s@%s", gPeerManager.peers[i].username, gPeerManager.peers[i].ip);
            currentErr = ParseIPv4(gPeerManager.peers[i].ip, &currentTargetIP); if (currentErr != noErr || currentTargetIP == 0) { log_message("Error (SendQuit): Could not parse peer IP '%s'. Skipping.", gPeerManager.peers[i].ip); lastErr = currentErr; goto NextPeerDelay; }
            log_to_file_only("SendQuit: Connecting to %s...", gPeerManager.peers[i].ip); currentErr = LowTCPActiveOpenSyncPoll(kConnectTimeoutTicks, currentTargetIP, PORT_TCP, giveTime);
            if (currentErr == noErr) { log_to_file_only("SendQuit: Connected successfully."); sendWDS[0].length = formattedLen; sendWDS[0].ptr = (Ptr)quitMessageBuffer; sendWDS[1].length = 0; sendWDS[1].ptr = NULL; log_to_file_only("SendQuit: Sending data..."); currentErr = LowTCPSendSyncPoll(kSendTimeoutTicks, true, (Ptr)sendWDS, giveTime); if (currentErr == noErr) { log_to_file_only("SendQuit: Send successful for %s.", gPeerManager.peers[i].ip); sentCount++; } else { log_message("Error (SendQuit): Send failed for %s: %d", gPeerManager.peers[i].ip, currentErr); lastErr = currentErr; } log_to_file_only("SendQuit: Aborting connection..."); OSErr abortErr = LowTCPAbortSyncPoll(giveTime); if (abortErr != noErr) { log_message("Warning (SendQuit): Abort failed for %s: %d", gPeerManager.peers[i].ip, abortErr); if (lastErr == noErr) lastErr = abortErr; } }
            else { log_message("Error (SendQuit): Connect failed for %s: %d", gPeerManager.peers[i].ip, currentErr); lastErr = currentErr; }
        NextPeerDelay: log_to_file_only("SendQuit: Yielding/Delaying (%d ticks) after peer %s...", kQuitLoopDelayTicks, gPeerManager.peers[i].ip); giveTime(); Delay(kQuitLoopDelayTicks, &dummyTimer); } }
SendQuitCleanup: log_message("TCP_SendQuitMessagesSync: Finished. Sent QUIT to %d out of %d active peers. Last error: %d.", sentCount, activePeerCount, lastErr); gNeedToStartListen = true; gIsSending = false; log_to_file_only("TCP_SendQuitMessagesSync: Released send lock."); return lastErr; }


// --- Private Helpers ---

// ProcessTCPReceive remains unchanged
static void ProcessTCPReceive(unsigned short dataLength) { char senderIPStrFromConnection[INET_ADDRSTRLEN], senderIPStrFromPayload[INET_ADDRSTRLEN]; char senderUsername[32], msgType[32], content[BUFFER_SIZE]; static tcp_platform_callbacks_t mac_callbacks = { .add_or_update_peer = mac_tcp_add_or_update_peer, .display_text_message = mac_tcp_display_text_message, .mark_peer_inactive = mac_tcp_mark_peer_inactive }; if (dataLength > 0 && gTCPRecvBuffer != NULL) { OSErr addrErr = AddrToStr(gPeerIP, senderIPStrFromConnection); if (addrErr != noErr) sprintf(senderIPStrFromConnection, "%lu.%lu.%lu.%lu", (gPeerIP >> 24) & 0xFF, (gPeerIP >> 16) & 0xFF, (gPeerIP >> 8) & 0xFF, gPeerIP & 0xFF); if (dataLength < kTCPRecvBufferSize) gTCPRecvBuffer[dataLength] = '\0'; else gTCPRecvBuffer[kTCPRecvBufferSize - 1] = '\0'; if (parse_message(gTCPRecvBuffer, dataLength, senderIPStrFromPayload, senderUsername, msgType, content) == 0) { log_to_file_only("ProcessTCPReceive: Calling shared handler for '%s' from %s@%s.", msgType, senderUsername, senderIPStrFromConnection); handle_received_tcp_message(senderIPStrFromConnection, senderUsername, msgType, content, &mac_callbacks, NULL); if (strcmp(msgType, MSG_QUIT) == 0) log_message("ProcessTCPReceive: QUIT received from %s. State machine will handle closure.", senderIPStrFromConnection); } else log_message("Failed to parse TCP message from %s (%u bytes). Discarding.", senderIPStrFromConnection, dataLength); } else if (dataLength == 0) log_to_file_only("ProcessTCPReceive: Received 0 bytes (likely connection closing signal)."); else log_message("ProcessTCPReceive: Error - dataLength > 0 but buffer is NULL?"); }

// StartAsyncListen - *** UPDATED CAST ***
static OSErr StartAsyncListen(void) {
    OSErr err; if (gTCPStream == NULL) return invalidStreamPtr; if (gCurrentPendingPB != NULL || gTCPState != TCP_STATE_IDLE) return inProgress;
    memset(&gListenPB, 0, sizeof(TCPiopb));
    // *** USE TCPIOCompletionProcPtr ***
    gListenPB.ioCompletion = (TCPIOCompletionProcPtr)ListenCompleteProc;
    gListenPB.ioCRefNum = gMacTCPRefNum; gListenPB.csCode = TCPPassiveOpen; gListenPB.tcpStream = gTCPStream;
    gListenPB.csParam.open.validityFlags = 0; gListenPB.csParam.open.localPort = PORT_TCP; gListenPB.csParam.open.commandTimeoutValue = 0;
    gTCPState = TCP_STATE_LISTENING; gCurrentPendingPB = &gListenPB; err = PBControlAsync((ParmBlkPtr)&gListenPB);
    if (err != noErr) { log_message("Error (StartAsyncListen): PBControlAsync failed immediately: %d", err); gCurrentPendingPB = NULL; gTCPState = TCP_STATE_IDLE; return err; } return 1;
}

// StartAsyncRecv - *** UPDATED CAST ***
static OSErr StartAsyncRecv(void) {
    OSErr err; if (gTCPStream == NULL) return invalidStreamPtr; if (gTCPRecvBuffer == NULL) return invalidBufPtr; if (gCurrentPendingPB != NULL || gTCPState != TCP_STATE_CONNECTED_IN) return inProgress;
    memset(&gRecvPB, 0, sizeof(TCPiopb));
    // *** USE TCPIOCompletionProcPtr ***
    gRecvPB.ioCompletion = (TCPIOCompletionProcPtr)RecvCompleteProc;
    gRecvPB.ioCRefNum = gMacTCPRefNum; gRecvPB.csCode = TCPRcv; gRecvPB.tcpStream = gTCPStream;
    gRecvPB.csParam.receive.rcvBuff = gTCPRecvBuffer; gRecvPB.csParam.receive.rcvBuffLen = kTCPRecvBufferSize; gRecvPB.csParam.receive.commandTimeoutValue = 0;
    gTCPState = TCP_STATE_RECEIVING; gCurrentPendingPB = &gRecvPB; err = PBControlAsync((ParmBlkPtr)&gRecvPB);
    if (err != noErr) { log_message("Error (StartAsyncRecv): PBControlAsync failed immediately: %d", err); gCurrentPendingPB = NULL; gTCPState = TCP_STATE_CONNECTED_IN; return err; } return 1;
}

// StartAsyncCloseIncoming - *** UPDATED CAST ***
static OSErr StartAsyncCloseIncoming(void) {
    OSErr err; if (gTCPStream == NULL) return invalidStreamPtr; if (gCurrentPendingPB != NULL || (gTCPState != TCP_STATE_CONNECTED_IN && gTCPState != TCP_STATE_RECEIVING)) return inProgress;
    memset(&gClosePB, 0, sizeof(TCPiopb));
    // *** USE TCPIOCompletionProcPtr ***
    gClosePB.ioCompletion = (TCPIOCompletionProcPtr)CloseCompleteProc;
    gClosePB.ioCRefNum = gMacTCPRefNum; gClosePB.csCode = TCPClose; gClosePB.tcpStream = gTCPStream;
    gClosePB.csParam.close.validityFlags = timeoutValue | timeoutAction; gClosePB.csParam.close.ulpTimeoutValue = kCloseTimeoutTicks; gClosePB.csParam.close.ulpTimeoutAction = AbortTrue;
    gTCPState = TCP_STATE_CLOSING_IN; gCurrentPendingPB = &gClosePB; err = PBControlAsync((ParmBlkPtr)&gClosePB);
    if (err != noErr) { log_message("Error (StartAsyncCloseIncoming): PBControlAsync failed immediately: %d. Assuming closed.", err); gCurrentPendingPB = NULL; gTCPState = TCP_STATE_IDLE; return noErr; } return 1;
}

// KillAsyncOperation remains unchanged
static OSErr KillAsyncOperation(GiveTimePtr giveTime) { OSErr killErr = noErr; TCPiopb *pbToKill = gCurrentPendingPB; TCPState stateWhenKilled = gTCPState; if (pbToKill != NULL && stateWhenKilled != TCP_STATE_IDLE) { log_message("KillAsyncOperation: Attempting async kill of pending operation (State: %d)...", stateWhenKilled); killErr = PBKillIO((ParmBlkPtr)pbToKill, false); if (killErr == noErr) { killErr = WaitForKillCompletion(giveTime, "KillAsyncOperation"); if (killErr != noErr) { log_message("KillAsyncOperation: Failed waiting for kill completion."); return streamBusyErr; } return noErr; } else if (killErr == paramErr) { log_message("CRITICAL (KillAsyncOperation): PBKillIO failed with paramErr (%d)! Cannot interrupt async op (State: %d).", killErr, stateWhenKilled); return streamBusyErr; } else { log_message("Error (KillAsyncOperation): PBKillIO failed immediately: %d (State: %d).", killErr, stateWhenKilled); if (gTCPState == TCP_STATE_IDLE && gCurrentPendingPB == NULL) { log_message("KillAsyncOperation: State is now IDLE, proceeding despite PBKillIO error."); return noErr; } return streamBusyErr; } } return noErr; }

// --- Low-Level Synchronous Polling TCP Helpers (for Sending) ---

// LowLevelSyncPoll remains unchanged
static OSErr LowLevelSyncPoll(TCPiopb *pBlock, GiveTimePtr giveTime, SInt16 csCode) { OSErr err; if (pBlock == NULL || giveTime == NULL) return paramErr; pBlock->ioCompletion = nil; pBlock->ioResult = 1; pBlock->csCode = csCode; err = PBControlAsync((ParmBlkPtr)pBlock); if (err != noErr) { log_message("Error (LowLevelSyncPoll %d): PBControlAsync failed immediately: %d", csCode, err); return err; } while (pBlock->ioResult > 0) { giveTime(); } return pBlock->ioResult; }
// LowTCPCreateSync remains unchanged
static OSErr LowTCPCreateSync(short macTCPRefNum, StreamPtr *streamPtr, Ptr connectionBuffer, unsigned long connBufferLen) { OSErr err; TCPiopb pbCreate; if (streamPtr == NULL || connectionBuffer == NULL) return paramErr; memset(&pbCreate, 0, sizeof(TCPiopb)); pbCreate.ioCompletion = nil; pbCreate.ioCRefNum = macTCPRefNum; pbCreate.csCode = TCPCreate; pbCreate.tcpStream = 0L; pbCreate.csParam.create.rcvBuff = connectionBuffer; pbCreate.csParam.create.rcvBuffLen = connBufferLen; pbCreate.csParam.create.notifyProc = nil; err = PBControlSync((ParmBlkPtr)&pbCreate); if (err == noErr) { *streamPtr = pbCreate.tcpStream; if (*streamPtr == NULL) { log_message("Error (LowTCPCreateSync): PBControlSync ok but returned NULL stream."); err = ioErr; } } else { *streamPtr = NULL; log_message("Error (LowTCPCreateSync): PBControlSync failed: %d", err); } return err; }
// LowTCPActiveOpenSyncPoll remains unchanged
static OSErr LowTCPActiveOpenSyncPoll(SInt8 ulpTimeoutTicks, ip_addr remoteHost, tcp_port remotePort, GiveTimePtr giveTime) { TCPiopb pb; if (gTCPStream == NULL) return invalidStreamPtr; memset(&pb, 0, sizeof(TCPiopb)); pb.ioCRefNum = gMacTCPRefNum; pb.tcpStream = gTCPStream; pb.csParam.open.ulpTimeoutValue = ulpTimeoutTicks; pb.csParam.open.ulpTimeoutAction = AbortTrue; pb.csParam.open.validityFlags = timeoutValue | timeoutAction; pb.csParam.open.commandTimeoutValue = 0; pb.csParam.open.remoteHost = remoteHost; pb.csParam.open.remotePort = remotePort; pb.csParam.open.localPort = 0; pb.csParam.open.localHost = 0; return LowLevelSyncPoll(&pb, giveTime, TCPActiveOpen); }
// LowTCPSendSyncPoll remains unchanged
static OSErr LowTCPSendSyncPoll(SInt8 ulpTimeoutTicks, Boolean push, Ptr wdsPtr, GiveTimePtr giveTime) { TCPiopb pb; if (gTCPStream == NULL) return invalidStreamPtr; if (wdsPtr == NULL) return invalidWDS; memset(&pb, 0, sizeof(TCPiopb)); pb.ioCRefNum = gMacTCPRefNum; pb.tcpStream = gTCPStream; pb.csParam.send.ulpTimeoutValue = ulpTimeoutTicks; pb.csParam.send.ulpTimeoutAction = AbortTrue; pb.csParam.send.validityFlags = timeoutValue | timeoutAction; pb.csParam.send.pushFlag = push; pb.csParam.send.urgentFlag = false; pb.csParam.send.wdsPtr = wdsPtr; return LowLevelSyncPoll(&pb, giveTime, TCPSend); }
// LowTCPAbortSyncPoll remains unchanged
static OSErr LowTCPAbortSyncPoll(GiveTimePtr giveTime) { OSErr err; TCPiopb pb; if (gTCPStream == NULL) { log_to_file_only("LowTCPAbortSyncPoll: Stream is NULL, nothing to abort."); return noErr; } memset(&pb, 0, sizeof(TCPiopb)); pb.ioCRefNum = gMacTCPRefNum; pb.tcpStream = gTCPStream; err = LowLevelSyncPoll(&pb, giveTime, TCPAbort); if (err == connectionDoesntExist || err == invalidStreamPtr) { log_to_file_only("LowTCPAbortSyncPoll: Abort completed (conn not exist/invalid stream). Result: %d", err); err = noErr; } else if (err != noErr) { log_message("Warning (LowTCPAbortSyncPoll): Abort poll failed: %d", err); } else { log_to_file_only("LowTCPAbortSyncPoll: Abort poll successful."); } return err; }
// LowTCPReleaseSync remains unchanged
static OSErr LowTCPReleaseSync(short macTCPRefNum, StreamPtr streamPtr) { OSErr err; TCPiopb pbRelease; if (streamPtr == NULL) return invalidStreamPtr; memset(&pbRelease, 0, sizeof(TCPiopb)); pbRelease.ioCompletion = nil; pbRelease.ioCRefNum = macTCPRefNum; pbRelease.csCode = TCPRelease; pbRelease.tcpStream = streamPtr; err = PBControlSync((ParmBlkPtr)&pbRelease); if (err != noErr && err != invalidStreamPtr) { log_message("Warning (LowTCPReleaseSync): PBControlSync failed: %d", err); } else if (err == invalidStreamPtr) { log_to_file_only("Info (LowTCPReleaseSync): Stream 0x%lX already invalid.", (unsigned long)streamPtr); err = noErr; } return err; }