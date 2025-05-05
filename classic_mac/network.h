//====================================
// FILE: ./classic_mac/network.h
//====================================

#ifndef NETWORK_H
#define NETWORK_H

#include <MacTypes.h>
#include "discovery.h" // Keep for OSErr definition if not elsewhere
#define __MACTCPCOMMONTYPES__ // Define before including MacTCP.h if needed by other headers
#include <AddressXlation.h> // For AddrToStr, StrToAddr if used directly
#include <MacTCP.h>         // For ip_addr, tcp_port etc.
#include "common_defs.h"

#define kTCPDriverName "\p.IPP"
#define ipctlGetAddr 15 // Control code to get local IP address

extern short gMacTCPRefNum; // Reference number for the MacTCP driver
extern ip_addr gMyLocalIP;  // Local IP address in network byte order
extern char gMyLocalIPStr[INET_ADDRSTRLEN]; // Local IP address as a C-string
extern char gMyUsername[32]; // User's chosen name

// Initializes MacTCP driver, DNR, UDP, and TCP listener.
// Returns noErr on success, or an OSErr otherwise.
OSErr InitializeNetworking(void);

// Cleans up TCP listener, UDP endpoint, DNR, and closes MacTCP driver.
void CleanupNetworking(void);

// Basic yield function for cooperative multitasking during sync network calls.
void YieldTimeToSystem(void);

// Helper to convert IP address string to network byte order ip_addr.
// Returns noErr on success, paramErr on failure.
OSErr ParseIPv4(const char *ip_str, ip_addr *out_addr);

#endif // NETWORK_H