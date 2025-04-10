// FILE: ./classic_mac/network.h
#ifndef NETWORK_H
#define NETWORK_H

#include <MacTypes.h>
#include <MacTCP.h>          // Include this first. It defines the necessary types.

#define __MACTCPCOMMONTYPES__ // Prevent AddressXlation.h from including conflicting types
#include <AddressXlation.h>  // Needs types defined in MacTCP.h

#include "common_defs.h" // For INET_ADDRSTRLEN

// --- Constants ---
#define kTCPDriverName "\p.IPP" // Pascal string for the IP driver

// --- Global Variables ---
extern short   gMacTCPRefNum; // Driver reference number for MacTCP
extern ip_addr gMyLocalIP;    // Our local IP address (network byte order)
extern char    gMyLocalIPStr[INET_ADDRSTRLEN]; // Our local IP as string

// --- Function Prototypes ---

/**
 * @brief Initializes MacTCP networking.
 * @details Opens the MacTCP driver (.IPP) and retrieves the local IP address.
 *          Stores the driver reference number and IP address in global variables.
 * @return OSErr noErr on success, or an error code from PBOpenSync, PBControlSync, or AddrToStr.
 */
OSErr InitializeNetworking(void);

/**
 * @brief Cleans up networking resources.
 * @details Closes the MacTCP driver if it was successfully opened.
 */
void CleanupNetworking(void);

// TODO: Add prototypes for UDP/TCP send/receive functions later

#endif // NETWORK_H