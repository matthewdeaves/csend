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
#define streamBusyErr (-23050) // Custom error: Cannot send, stream busy or kill failed

// --- TCP Stream State ---
// Describes the current activity of the SINGLE shared stream using Async operations.
typedef enum {
    TCP_STATE_UNINITIALIZED,    // Before InitTCP completes
    TCP_STATE_IDLE,             // Stream created, ready to start listen or send
    TCP_STATE_LISTENING,        // Async TCPPassiveOpen pending
    TCP_STATE_CONNECTED_IN,     // Incoming connection established by ListenCompleteProc, ready for PollTCP to start Rcv
    TCP_STATE_RECEIVING,        // Async TCPRcv pending
    TCP_STATE_CLOSING_IN,       // Async TCPClose pending (for incoming connection)
    TCP_STATE_RELEASING,        // TCPRelease pending (during cleanup)
    TCP_STATE_ERROR             // An unrecoverable error occurred
    // Note: Sending state is handled by gIsSending flag + sync helpers
} TCPState;


// --- Completion Routine Prototypes ---
// These MUST be declared pascal for MacTCP callback convention
pascal void ListenCompleteProc(struct TCPiopb *iopb);
pascal void RecvCompleteProc(struct TCPiopb *iopb);
pascal void CloseCompleteProc(struct TCPiopb *iopb);
// We might need one for KillIO completion? MacTCP docs don't specify one for PBKillIO. Assume it's synchronous enough or the original completion runs.

// Initializes the single persistent TCP stream.
// Returns noErr on success, OSErr otherwise.
OSErr InitTCP(short macTCPRefNum);

// Cleans up the single persistent TCP stream.
void CleanupTCP(short macTCPRefNum);

// Polls the SINGLE stream state machine (checks flags set by completion routines).
// MUST be called regularly from the main event loop.
void PollTCP(void); // No GiveTimePtr needed here

// Sends a text message SYNCHRONOUSLY to a peer using the single stream.
// Attempts to kill pending async ops first.
// Returns noErr on success, streamBusyErr if busy/kill fails, or other OSErr on failure.
OSErr TCP_SendTextMessageSync(const char *peerIP, const char *message, GiveTimePtr giveTime);

// Sends QUIT messages SYNCHRONOUSLY to all active peers using the single stream,
// with delays between each peer's send attempt.
// Attempts to kill pending async ops first.
// Returns streamBusyErr if busy/kill fails initially, or the last error encountered (noErr if all succeed).
OSErr TCP_SendQuitMessagesSync(GiveTimePtr giveTime);

// --- Public State Access (Optional, for debugging/info) ---
TCPState GetTCPState(void);

#endif // TCP_H