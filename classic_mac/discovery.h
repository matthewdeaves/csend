#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <MacTypes.h>
#include <Devices.h>
#include <MacTCP.h> // Includes UDPiopb definition

// Keep original defines for clarity if preferred
#define udpCreate UDPCreate
#define udpRead UDPRead
#define udpBfrReturn UDPBfrReturn
#define udpWrite UDPWrite
#define udpRelease UDPRelease

#include "common_defs.h" // For BUFFER_SIZE, PORT_UDP etc.
#include "peer_mac.h"    // For peer_t definition if needed (likely not directly here)

#define BROADCAST_IP 0xFFFFFFFFUL
#define kMinUDPBufSize 2048 // Keep a reasonable buffer size

// --- Global State for Asynchronous UDP ---

extern StreamPtr gUDPStream;            // UDP Stream identifier
extern Ptr gUDPRecvBuffer;          // Buffer allocated for receiving data
extern UDPiopb gUDPReadPB;            // Persistent PB for udpRead calls
// extern UDPiopb gUDPWritePB;        // No longer needed for sync writes
extern UDPiopb gUDPBfrReturnPB;       // Persistent PB for udpBfrReturn calls

extern Boolean gUDPReadPending;       // Is an asynchronous udpRead currently active?
// extern Boolean gUDPWritePending;   // No longer needed for sync writes
extern Boolean gUDPBfrReturnPending;  // Is an asynchronous udpBfrReturn currently active?

// Flags/Data set by completion routines for the main loop to process (NOT USED)
// extern volatile Boolean gUDPDataAvailable;
// extern volatile OSErr gUDPReadResult;
// extern volatile Boolean gUDPSendComplete;
// extern volatile OSErr gUDPSendResult;
// extern volatile Boolean gUDPBfrReturnComplete;
// extern volatile OSErr gUDPBfrReturnResult;

// --- Original Timer ---
extern unsigned long gLastBroadcastTimeTicks; // For periodic broadcast timing

// --- Function Prototypes ---

// Initialization and Cleanup
OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum);
void CleanupUDPDiscoveryEndpoint(short macTCPRefNum);

// Initiating Operations
OSErr StartAsyncUDPRead(void); // Initiates a non-blocking read (NO COMPLETION)
OSErr SendDiscoveryBroadcastSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr); // Renamed to Sync
OSErr SendDiscoveryResponseSync(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr, ip_addr destIP, udp_port destPort); // Renamed to Sync
OSErr ReturnUDPBufferAsync(Ptr dataPtr, unsigned short bufferSize); // Pass the *original* buffer size (NO COMPLETION)

// Processing Results (called from main loop based on polling)
void ProcessUDPReceive(short macTCPRefNum, ip_addr myLocalIP); // Handles parsing, responding, peer updates

// Periodic Check (called from main loop)
void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr); // Still needed for timing the initiation

// Completion Routines (NOT USED)
// pascal void UDPReadComplete(UDPiopb *pb);
// pascal void UDPWriteComplete(UDPiopb *pb);
// pascal void UDPBfrReturnComplete(UDPiopb *pb);


#endif // DISCOVERY_H