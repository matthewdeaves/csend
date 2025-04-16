// FILE: ./classic_mac/network.h
#ifndef NETWORK_H
#define NETWORK_H

#include <MacTCP.h>
#include <MacTypes.h>


#define __MACTCPCOMMONTYPES__ // Prevent AddressXlation.h from including conflicting types
#include <AddressXlation.h>  // Needs types defined in MacTCP.h

#include "common_defs.h" // For INET_ADDRSTRLEN

// --- Constants ---
#define kTCPDriverName "\p.IPP" // Pascal string for the IP driver
// UDP constants moved to discovery.h

// --- MacTCP Control Codes (General - Add more as needed) ---
#define ipctlGetAddr 15 // For GetMyIPAddr
// UDP codes moved to discovery.h

// --- Global Variables ---
extern short   gMacTCPRefNum; // Driver reference number for MacTCP
extern ip_addr gMyLocalIP;    // Our local IP address (network byte order)
extern char    gMyLocalIPStr[INET_ADDRSTRLEN]; // Our local IP as string
// UDP globals moved to discovery.h

// --- Function Prototypes ---

/**
 * @brief Initializes MacTCP networking (Driver, IP Address, DNR).
 * @return OSErr noErr on success, or an error code.
 */
OSErr InitializeNetworking(void);

/**
 * @brief Cleans up general networking resources (DNR, Driver).
 *        Calls UDP cleanup internally.
 */
void CleanupNetworking(void);

// UDP function prototypes moved to discovery.h

#endif // NETWORK_H