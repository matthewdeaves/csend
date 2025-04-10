// FILE: ./classic_mac/network.h
#ifndef NETWORK_H
#define NETWORK_H

#include <MacTypes.h>
#include <MacTCP.h>          // Include this first. It defines the necessary types.
#include <MixedMode.h>       // ADDED: For UniversalProcPtr and UPP routines

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

// Peer List Globals (Classic Mac doesn't use app_state_t)
extern peer_t  gPeerList[MAX_PEERS];
extern short   gPeerCount; // Number of active peers currently in the list

// UDP Read Globals
extern UDPiopb gUDPReadPB;           // Parameter block for the asynchronous UDP read
extern char    gUDPRecvBuffer[BUFFER_SIZE]; // Buffer for receiving UDP data (for parsed data)
extern Boolean gUDPReadPending;      // Flag indicating if an async read is outstanding
extern Ptr     gUDPReceiveAreaPtr;   // Pointer to dynamically allocated receive area
extern UDPIOCompletionUPP gUDPReadCompletionUPP; // ADDED: Global UPP for completion routine

// --- Function Prototypes ---

// Initialization and Cleanup
OSErr InitializeNetworking(void);
void CleanupNetworking(void);

// UDP Discovery
OSErr InitUDPDiscovery(void);
OSErr SendDiscoveryBroadcast(void);
void CheckSendBroadcast(void);
OSErr IssueUDPRead(void); // Function to issue the async read
void ProcessUDPReceive(void); // Function to process completed read
pascal void UDPReadCompletion(UDPiopb *pb); // Completion routine (implementation in .c)
OSErr SendUDPResponse(ip_addr destIP, udp_port destPort); // Send DISCOVERY_RESPONSE

// Peer Management
void InitPeerList(void);
int AddOrUpdatePeer(const char *ip, const char *username); // Returns 1 if new, 0 if updated, -1 if full
short FindPeerByIP(const char *ip); // Returns index or -1
void PruneInactivePeers(void); // Function to mark timed-out peers inactive

#endif // NETWORK_H