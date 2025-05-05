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

// --- TCP Listener Stream State ---
// Describes the current activity of the dedicated LISTENER stream.
typedef enum {
    TCP_LSTATE_UNINITIALIZED, // Before InitTCP completes
    TCP_LSTATE_IDLE,          // Stream created, ready to listen
    TCP_LSTATE_LISTENING,     // TCPPassiveOpen pending
    TCP_LSTATE_RECEIVING,     // TCPRcv pending (incoming connection active)
    TCP_LSTATE_CLOSING,       // TCPClose pending (graceful close of incoming connection)
    TCP_LSTATE_RELEASING,     // TCPRelease pending (during cleanup)
    TCP_LSTATE_ERROR          // An unrecoverable error occurred
} TCPListenerState;


// Initializes the persistent TCP listener stream and starts listening.
// Returns noErr on success, OSErr otherwise.
OSErr InitTCP(short macTCPRefNum);

// Cleans up the persistent TCP listener stream.
void CleanupTCP(short macTCPRefNum);

// Polls the LISTENER stream state machine (for incoming connections/data).
// MUST be called regularly from the main event loop.
void PollTCPListener(short macTCPRefNum);

// Sends a text message SYNCHRONOUSLY to a peer using a temporary stream.
// Returns noErr on success, OSErr otherwise.
OSErr TCP_SendTextMessageSync(const char *peerIP, const char *message, GiveTimePtr giveTime);

// Sends QUIT messages SYNCHRONOUSLY to all active peers using temporary streams,
// with delays between each peer's send attempt.
// Returns the last error encountered (noErr if all succeed).
OSErr TCP_SendQuitMessagesSync(GiveTimePtr giveTime);

// --- Public State Access (Optional, for debugging/info) ---
TCPListenerState GetTCPListenerState(void); // Function to get the current listener state

#endif // TCP_H