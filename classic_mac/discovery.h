// FILE: ./classic_mac/discovery.h
#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <MacTCP.h> // For StreamPtr, UDPiopb, wdsEntry etc.
#include <MacTypes.h>

#include "common_defs.h" // For PORT_UDP, DISCOVERY_INTERVAL, BUFFER_SIZE, INET_ADDRSTRLEN
#include "peer_mac.h"    // For AddOrUpdatePeer

// --- Constants ---
#define BROADCAST_IP 0xFFFFFFFFUL // Standard broadcast address (255.255.255.255)
#define kMinUDPBufSize  2048        // Minimum receive buffer size for UDPCreate

// --- MacTCP UDP Control Codes (Specific to Discovery) ---
// Note: These are also defined in network.h for general use,
// but defining them here makes discovery.c self-contained regarding its specific needs.
// Consider creating a central MacTCP constants header if more modules use them.
#define udpCreate       20
#define udpRead         21
#define udpBfrReturn    22
#define udpWrite        23
#define udpRelease      24

// --- Global Variables (External Declarations) ---
// These are defined in discovery.c but needed by other modules (like main.c)
extern StreamPtr gUDPStream; // Pointer to our UDP stream for discovery
extern Ptr     gUDPRecvBuffer; // Pointer to the buffer allocated for UDPCreate
extern unsigned long gLastBroadcastTimeTicks; // Time of last broadcast in Ticks

// --- Function Prototypes ---

/**
 * @brief Initializes the UDP endpoint for discovery broadcasts and receives using PBControl.
 * @param macTCPRefNum The driver reference number obtained from PBOpen.
 * @return OSErr noErr on success, or an error code on failure.
 */
OSErr InitUDPDiscoveryEndpoint(short macTCPRefNum); // Renamed for clarity

/**
 * @brief Sends a UDP discovery broadcast message using PBControl.
 * @param macTCPRefNum The driver reference number.
 * @param myUsername The username to include in the broadcast.
 * @param myLocalIPStr The local IP address string to include.
 * @return OSErr noErr on success, or an error code on failure.
 */
OSErr SendDiscoveryBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr);

/**
 * @brief Checks if it's time to send the next discovery broadcast and sends if needed.
 * @param macTCPRefNum The driver reference number.
 * @param myUsername The username to include in the broadcast.
 * @param myLocalIPStr The local IP address string to include.
 */
void CheckSendBroadcast(short macTCPRefNum, const char *myUsername, const char *myLocalIPStr);

/**
 * @brief Checks for and processes incoming UDP packets (e.g., discovery responses).
 *        Uses PBControlSync with a short timeout for non-blocking checks.
 * @param macTCPRefNum The driver reference number.
 * @param myLocalIP The local IP address (numeric) to ignore self-messages.
 */
void CheckUDPReceive(short macTCPRefNum, ip_addr myLocalIP); // Added function prototype

/**
 * @brief Cleans up the UDP discovery endpoint resources.
 * @param macTCPRefNum The driver reference number.
 */
void CleanupUDPDiscoveryEndpoint(short macTCPRefNum); // Renamed for clarity


#endif // DISCOVERY_H