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

// Initializes the persistent TCP listener stream.
// Returns noErr on success, OSErr otherwise.
OSErr InitTCPListener(short macTCPRefNum);

// Cleans up the persistent TCP listener stream.
void CleanupTCPListener(short macTCPRefNum);

// Polls the listener stream for incoming connections and data.
// MUST be called regularly from the main event loop.
void PollTCPListener(short macTCPRefNum, ip_addr myLocalIP);

// Sends a text message synchronously to a peer.
// Creates a temporary stream, connects, sends, aborts, and releases.
// Returns noErr on success, OSErr otherwise.
OSErr TCP_SendTextMessageSync(const char *peerIP, const char *message, GiveTimePtr giveTime);

// Sends QUIT messages synchronously to all active peers.
// Uses temporary streams for each peer.
// Returns the last error encountered (noErr if all succeed).
OSErr TCP_SendQuitMessagesSync(GiveTimePtr giveTime);

// -- State related to the single incoming connection on the listener stream --
extern ip_addr gCurrentConnectionIP;   // IP of the currently connected peer (incoming)
extern tcp_port gCurrentConnectionPort; // Port of the currently connected peer (incoming)
extern Boolean gIsConnectionActive;    // Is there an active incoming connection?

#endif // TCP_H
