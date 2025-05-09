#ifndef NETWORK_H
#define NETWORK_H
#include <MacTypes.h>
#include "discovery.h"
#define __MACTCPCOMMONTYPES__
#include <AddressXlation.h>
#include <MacTCP.h>
#include "common_defs.h"
#define kTCPDriverName "\p.IPP"
#define ipctlGetAddr 15
extern short gMacTCPRefNum;
extern ip_addr gMyLocalIP;
extern char gMyLocalIPStr[INET_ADDRSTRLEN];
extern char gMyUsername[32];
OSErr InitializeNetworking(void);
void CleanupNetworking(void);
void YieldTimeToSystem(void);
OSErr ParseIPv4(const char *ip_str, ip_addr *out_addr);
#endif
