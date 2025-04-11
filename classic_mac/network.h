// FILE: ./classic_mac/network.h
#ifndef NETWORK_H
#define NETWORK_H

#include <MacTypes.h>
#include <MacTCP.h>          // Include this first. It defines the necessary types.

#define __MACTCPCOMMONTYPES__ // Prevent AddressXlation.h from including conflicting types
#include <AddressXlation.h>  // Needs types defined in MacTCP.h

#include "common_defs.h" // For INET_ADDRSTRLEN, PORT_UDP, DISCOVERY_INTERVAL

// --- Constants ---
#define kTCPDriverName "\p.IPP" // Pascal string for the IP driver
#define BROADCAST_IP 0xFFFFFFFFUL // Standard broadcast address (255.255.255.255)
#define kMinUDPBufSize  2048        // Minimum receive buffer size for UDPCreate

// --- MacTCP UDP Control Codes (Corrected based on MacTCP Programmer's Guide Appendix) ---
#define udpCreate       20
#define udpWrite        23 // Corrected from example's udpSend
#define udpRelease      24
// #define udpRead         21 // Not needed now
// #define udpBfrReturn    22 // Not needed now
// #define udpMaxMTUSize   25 // Not needed now
// #define udpStatus       26 // Not needed now

// --- Global Variables ---
extern short   gMacTCPRefNum; // Driver reference number for MacTCP
extern ip_addr gMyLocalIP;    // Our local IP address (network byte order)
extern char    gMyLocalIPStr[INET_ADDRSTRLEN]; // Our local IP as string
extern StreamPtr gUDPStream; // Pointer to our UDP stream for broadcasting
extern Ptr     gUDPRecvBuffer; // Pointer to the buffer allocated for UDPCreate
extern unsigned long gLastBroadcastTimeTicks; // Time of last broadcast in Ticks

// --- Function Prototypes ---

/**
 * @brief Initializes MacTCP networking (Driver, IP Address, DNR).
 */
OSErr InitializeNetworking(void);

/**
 * @brief Initializes the UDP endpoint for discovery broadcasts using PBControl.
 */
OSErr InitUDPBroadcastEndpoint(void); // Renamed for clarity

/**
 * @brief Sends a UDP discovery broadcast message using PBControl.
 */
OSErr SendDiscoveryBroadcast(void);

/**
 * @brief Checks if it's time to send the next discovery broadcast.
 */
void CheckSendBroadcast(void);


/**
 * @brief Cleans up networking resources (DNR, Driver, UDP Endpoint).
 */
void CleanupNetworking(void);

#endif // NETWORK_H