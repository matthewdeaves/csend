// FILE: ./classic_mac/network.h
#ifndef NETWORK_H
#define NETWORK_H

#include <MacTypes.h>
#include <MacTCP.h>          // Include this first. It defines the necessary types.
#include <MixedMode.h>       // For UniversalProcPtr and UPP routines

#define __MACTCPCOMMONTYPES__ // Prevent AddressXlation.h from including conflicting types
#include <AddressXlation.h>  // Needs types defined in MacTCP.h

#include "common_defs.h" // For INET_ADDRSTRLEN, PORT_UDP, DISCOVERY_INTERVAL, peer_t

// --- Constants ---
#define kTCPDriverName "\p.IPP" // Pascal string for the IP driver
#define BROADCAST_IP 0xFFFFFFFFUL // Standard broadcast address (255.255.255.255)
#define kUDPReceiveBufferSize (2 * BUFFER_SIZE + 256) // Define buffer size constant

// --- MacTCP UDP Control Codes ---
#define udpCreate       20
#define udpSend         23
#define udpRelease      24
#define udpRead         21
#define udpBfrReturn    22
#define udpMaxMTUSize   25
#define udpStatus       26

// --- Global Variables ---
extern short   gMacTCPRefNum; // Driver reference number for MacTCP
extern ip_addr gMyLocalIP;    // Our local IP address (network byte order)
extern char    gMyLocalIPStr[INET_ADDRSTRLEN]; // Our local IP as string
extern unsigned long gLastBroadcastTimeTicks; // Time of last broadcast in Ticks

// Peer List Globals
extern peer_t  gPeerList[MAX_PEERS];
extern short   gPeerCount;

// UDP Read Globals
extern UDPiopb gUDPReadPB;
extern char    gUDPRecvBuffer[BUFFER_SIZE]; // Temp buffer for holding *parsed* data if needed
extern Boolean gUDPReadPending;
extern Ptr     gUDPReceiveAreaPtr;
extern UDPIOCompletionUPP gUDPReadCompletionUPP;

// --- ADDED: State for Deferred Processing ---
extern Boolean gNeedToSendResponse; // Flag: Do we need to send a DISCOVERY_RESPONSE?
extern ip_addr gResponseDestIP;     // IP to send response to
extern udp_port gResponseDestPort;  // Port to send response to

// --- Function Prototypes ---

// Initialization and Cleanup
OSErr InitializeNetworking(void);
void CleanupNetworking(void);

// UDP Discovery
OSErr InitUDPDiscovery(void);
OSErr SendDiscoveryBroadcast(void);
void CheckSendBroadcast(void);
OSErr IssueUDPRead(void);
void ProcessUDPReceive(void); // Processes completed read, sets flags
pascal void UDPReadCompletion(UDPiopb *pb);
OSErr SendUDPResponse(ip_addr destIP, udp_port destPort); // Sends the response
void CheckAndSendDeferredResponse(void); // ADDED: Called from main loop

// Peer Management
void InitPeerList(void);
int AddOrUpdatePeer(const char *ip, const char *username);
short FindPeerByIP(const char *ip);
void PruneInactivePeers(void);

#endif // NETWORK_H