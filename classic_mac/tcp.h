//====================================
// FILE: ./classic_mac/tcp.h
//====================================

#ifndef TCP_H
#define TCP_H

#include <MacTypes.h>
#include <MacTCP.h>
#include "common_defs.h"

// Define GiveTimePtr if not defined elsewhere (e.g., network.h)
typedef void (*GiveTimePtr)(void);

#define kTCPDefaultTimeout 0 // Default ULP timeout (0 = infinite)
#define streamBusyErr (-23050) // Custom error: Cannot send, stream busy (receiving or sending)

// --- TCP Stream State (Synchronous Polling Model) ---
// Describes the current activity of the SINGLE shared stream.
typedef enum {
    TCP_STATE_UNINITIALIZED,    // Before InitTCP completes
    TCP_STATE_IDLE,             // Stream created, ready to poll for listen or initiate send
    TCP_STATE_LISTENING_POLL,   // TCPPassiveOpen sync poll in progress
    TCP_STATE_CONNECTED_IN,     // Incoming connection established, ready to Rcv/Status poll
    // Note: Sending state is handled by gIsSending flag + sync helpers (ActiveOpen, Send, Abort)
    // TCP_STATE_SENDING,       // (Conceptual, not an explicit state in this model's gTCPState)
    // TCP_STATE_ABORTING,      // (Conceptual, not an explicit state in this model's gTCPState)
    TCP_STATE_RELEASING,        // TCPRelease pending (during cleanup)
    TCP_STATE_ERROR             // An unrecoverable error occurred (e.g., stream invalid)
} TCPState;


// Initializes the single persistent TCP stream.
// Returns noErr on success, OSErr otherwise.
OSErr InitTCP(short macTCPRefNum);

// Cleans up the single persistent TCP stream.
void CleanupTCP(short macTCPRefNum);

// Polls the SINGLE stream state machine (for incoming connections/data using synchronous polling).
// MUST be called regularly from the main event loop.
void PollTCP(GiveTimePtr giveTime); // Takes GiveTimePtr for internal polling loops

// Sends a text message SYNCHRONOUSLY to a peer using the single stream.
// Returns noErr on success, streamBusyErr if busy, or other OSErr on failure.
OSErr TCP_SendTextMessageSync(const char *peerIP, const char *message, GiveTimePtr giveTime);

// Sends QUIT messages SYNCHRONOUSLY to all active peers using the single stream,
// with delays between each peer's send attempt.
// Returns streamBusyErr if busy initially, or the last error encountered (noErr if all succeed).
OSErr TCP_SendQuitMessagesSync(GiveTimePtr giveTime);

// --- Public State Access (Optional, for debugging/info) ---
TCPState GetTCPState(void);

#endif // TCP_H